/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>

#include "vaapi.h"
#include "common/common.h"
#include "common/msg.h"
#include "mp_image.h"
#include "img_format.h"
#include "mp_image_pool.h"

bool check_va_status(struct mp_log *log, VAStatus status, const char *msg)
{
    if (status != VA_STATUS_SUCCESS) {
        mp_err(log, "%s: %s\n", msg, vaErrorStr(status));
        return false;
    }
    return true;
}

int va_get_colorspace_flag(enum mp_csp csp)
{
#if USE_VAAPI_COLORSPACE
    switch (csp) {
    case MP_CSP_BT_601:         return VA_SRC_BT601;
    case MP_CSP_BT_709:         return VA_SRC_BT709;
    case MP_CSP_SMPTE_240M:     return VA_SRC_SMPTE_240;
    }
#endif
    return 0;
}

struct fmtentry {
    uint32_t va;
    enum mp_imgfmt mp;
};

static const struct fmtentry va_to_imgfmt[] = {
    {VA_FOURCC_YV12, IMGFMT_420P},
    {VA_FOURCC_I420, IMGFMT_420P},
    {VA_FOURCC_IYUV, IMGFMT_420P},
    {VA_FOURCC_NV12, IMGFMT_NV12},
    {VA_FOURCC_UYVY, IMGFMT_UYVY},
    {VA_FOURCC_YUY2, IMGFMT_YUYV},
    // Note: not sure about endian issues (the mp formats are byte-addressed)
    {VA_FOURCC_RGBA, IMGFMT_RGBA},
    {VA_FOURCC_RGBX, IMGFMT_RGBA},
    {VA_FOURCC_BGRA, IMGFMT_BGRA},
    {VA_FOURCC_BGRX, IMGFMT_BGRA},
    {0             , IMGFMT_NONE}
};

enum mp_imgfmt va_fourcc_to_imgfmt(uint32_t fourcc)
{
    for (const struct fmtentry *entry = va_to_imgfmt; entry->va; ++entry) {
        if (entry->va == fourcc)
            return entry->mp;
    }
    return IMGFMT_NONE;
}

uint32_t va_fourcc_from_imgfmt(int imgfmt)
{
    for (const struct fmtentry *entry = va_to_imgfmt; entry->va; ++entry) {
        if (entry->mp == imgfmt)
            return entry->va;
    }
    return 0;
}

struct va_image_formats {
    VAImageFormat *entries;
    int num;
};

static void va_get_formats(struct mp_vaapi_ctx *ctx)
{
    int num = vaMaxNumImageFormats(ctx->display);
    VAImageFormat entries[num];
    VAStatus status = vaQueryImageFormats(ctx->display, entries, &num);
    if (!CHECK_VA_STATUS(ctx, "vaQueryImageFormats()"))
        return;
    struct va_image_formats *formats = talloc_ptrtype(ctx, formats);
    formats->entries = talloc_array(formats, VAImageFormat, num);
    formats->num = num;
    MP_VERBOSE(ctx, "%d image formats available:\n", num);
    for (int i = 0; i < num; i++) {
        formats->entries[i] = entries[i];
        MP_VERBOSE(ctx, "  %s\n", VA_STR_FOURCC(entries[i].fourcc));
    }
    ctx->image_formats = formats;
}

struct mp_vaapi_ctx *va_initialize(VADisplay *display, struct mp_log *plog)
{
    struct mp_vaapi_ctx *res = NULL;
    struct mp_log *log = mp_log_new(NULL, plog, "/vaapi");
    int major_version, minor_version;
    int status = vaInitialize(display, &major_version, &minor_version);
    if (!check_va_status(log, status, "vaInitialize()"))
        goto error;

    mp_verbose(log, "VA API version %d.%d\n", major_version, minor_version);

    res = talloc_ptrtype(NULL, res);
    *res = (struct mp_vaapi_ctx) {
        .log = talloc_steal(res, log),
        .display = display,
    };

    va_get_formats(res);
    if (!res->image_formats)
        goto error;
    return res;

error:
    if (res && res->display)
        vaTerminate(res->display);
    talloc_free(log);
    talloc_free(res);
    return NULL;
}

// Undo va_initialize, and close the VADisplay.
void va_destroy(struct mp_vaapi_ctx *ctx)
{
    if (ctx) {
        if (ctx->display)
            vaTerminate(ctx->display);
        talloc_free(ctx);
    }
}

VAImageFormat *va_image_format_from_imgfmt(const struct va_image_formats *formats,
                                           int imgfmt)
{
    const int fourcc = va_fourcc_from_imgfmt(imgfmt);
    if (!formats || !formats->num || !fourcc)
        return NULL;
    for (int i = 0; i < formats->num; i++) {
        if (formats->entries[i].fourcc == fourcc)
            return &formats->entries[i];
    }
    return NULL;
}

typedef struct va_surface_priv {
    struct mp_vaapi_ctx *ctx;
    VADisplay display;
    VAImage image;       // used for software decoding case
    bool is_derived;     // is image derived by vaDeriveImage()?
} va_surface_priv_t;

static void va_surface_destroy(struct va_surface *surface)
{
    if (!surface)
        return;
    if (surface->id != VA_INVALID_ID) {
        va_surface_priv_t *p = surface->p;
        if (p->image.image_id != VA_INVALID_ID)
            vaDestroyImage(p->display, p->image.image_id);
        vaDestroySurfaces(p->display, &surface->id, 1);
    }
    talloc_free(surface);
}

static void release_va_surface(void *arg)
{
    struct va_surface *surface = arg;
    va_surface_destroy(surface);
}

static struct mp_image *alloc_surface(struct mp_vaapi_ctx *ctx, int rt_format,
                                      int w, int h)
{
    VASurfaceID id = VA_INVALID_ID;
    VAStatus status;
    status = vaCreateSurfaces(ctx->display, w, h, rt_format, 1, &id);
    if (!CHECK_VA_STATUS(ctx, "vaCreateSurfaces()"))
        return NULL;

    struct va_surface *surface = talloc_ptrtype(NULL, surface);
    if (!surface)
        return NULL;

    surface->id = id;
    surface->w = w;
    surface->h = h;
    surface->rt_format = rt_format;
    surface->p = talloc_zero(surface, va_surface_priv_t);
    surface->p->ctx = ctx;
    surface->p->display = ctx->display;
    surface->p->image.image_id = surface->p->image.buf = VA_INVALID_ID;

    struct mp_image img = {0};
    mp_image_setfmt(&img, IMGFMT_VAAPI);
    mp_image_set_size(&img, surface->w, surface->h);
    img.planes[0] = (uint8_t*)surface;
    img.planes[3] = (uint8_t*)(uintptr_t)surface->id;
    return mp_image_new_custom_ref(&img, surface, release_va_surface);
}

static void va_surface_image_destroy(struct va_surface *surface)
{
    if (!surface || surface->p->image.image_id == VA_INVALID_ID)
        return;
    va_surface_priv_t *p = surface->p;
    vaDestroyImage(p->display, p->image.image_id);
    p->image.image_id = VA_INVALID_ID;
    p->is_derived = false;
}

static VAImage *va_surface_image_alloc(struct va_surface *surface,
                                       VAImageFormat *format)
{
    if (!format || !surface)
        return NULL;
    va_surface_priv_t *p = surface->p;
    if (p->image.image_id != VA_INVALID_ID &&
        p->image.format.fourcc == format->fourcc)
        return &p->image;
    va_surface_image_destroy(surface);

    VAStatus status = vaDeriveImage(p->display, surface->id, &p->image);
    if (status == VA_STATUS_SUCCESS) {
        /* vaDeriveImage() is supported, check format */
        if (p->image.format.fourcc == format->fourcc &&
                p->image.width == surface->w && p->image.height == surface->h) {
            p->is_derived = true;
            MP_VERBOSE(p->ctx, "Using vaDeriveImage()\n");
        } else {
            vaDestroyImage(p->display, p->image.image_id);
            status = VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    if (status != VA_STATUS_SUCCESS) {
        p->image.image_id = VA_INVALID_ID;
        status = vaCreateImage(p->display, format, surface->w, surface->h,
                               &p->image);
        if (!CHECK_VA_STATUS(p->ctx, "vaCreateImage()")) {
            p->image.image_id = VA_INVALID_ID;
            return NULL;
        }
    }
    return &surface->p->image;
}

// img must be a VAAPI surface; make sure its internal VAImage is allocated
// to a format corresponding to imgfmt (or return an error).
int va_surface_image_alloc_imgfmt(struct mp_image *img, int imgfmt)
{
    struct va_surface *surface = va_surface_in_mp_image(img);
    if (!surface)
        return -1;
    VAImageFormat *format =
        va_image_format_from_imgfmt(surface->p->ctx->image_formats, imgfmt);
    if (!format)
        return -1;
    if (!va_surface_image_alloc(surface, format))
        return -1;
    return 0;
}

VASurfaceID va_surface_id_in_mp_image(const struct mp_image *mpi)
{
    return mpi && mpi->imgfmt == IMGFMT_VAAPI ?
        (VASurfaceID)(uintptr_t)mpi->planes[3] : VA_INVALID_ID;
}

struct va_surface *va_surface_in_mp_image(struct mp_image *mpi)
{
    return mpi && mpi->imgfmt == IMGFMT_VAAPI ?
        (struct va_surface*)mpi->planes[0] : NULL;
}

VASurfaceID va_surface_id(const struct va_surface *surface)
{
    return surface ? surface->id : VA_INVALID_ID;
}

bool va_image_map(struct mp_vaapi_ctx *ctx, VAImage *image, struct mp_image *mpi)
{
    int imgfmt = va_fourcc_to_imgfmt(image->format.fourcc);
    if (imgfmt == IMGFMT_NONE)
        return false;
    void *data = NULL;
    const VAStatus status = vaMapBuffer(ctx->display, image->buf, &data);
    if (!CHECK_VA_STATUS(ctx, "vaMapBuffer()"))
        return false;

    *mpi = (struct mp_image) {0};
    mp_image_setfmt(mpi, imgfmt);
    mp_image_set_size(mpi, image->width, image->height);

    for (int p = 0; p < image->num_planes; p++) {
        mpi->stride[p] = image->pitches[p];
        mpi->planes[p] = (uint8_t *)data + image->offsets[p];
    }

    if (image->format.fourcc == VA_FOURCC_YV12) {
        MPSWAP(unsigned int, mpi->stride[1], mpi->stride[2]);
        MPSWAP(uint8_t *, mpi->planes[1], mpi->planes[2]);
    }

    return true;
}

bool va_image_unmap(struct mp_vaapi_ctx *ctx, VAImage *image)
{
    const VAStatus status = vaUnmapBuffer(ctx->display, image->buf);
    return CHECK_VA_STATUS(ctx, "vaUnmapBuffer()");
}

bool va_surface_upload(struct va_surface *surface, struct mp_image *mpi)
{
    va_surface_priv_t *p = surface->p;

    VAImageFormat *format =
        va_image_format_from_imgfmt(p->ctx->image_formats, mpi->imgfmt);
    if (!format)
        return false;
    if (!va_surface_image_alloc(surface, format))
        return false;

    struct mp_image img;
    if (!va_image_map(p->ctx, &p->image, &img))
        return false;
    mp_image_copy(&img, mpi);
    va_image_unmap(p->ctx, &p->image);

    if (!p->is_derived) {
        VAStatus status = vaPutImage2(p->display, surface->id,
                                      p->image.image_id,
                                      0, 0, mpi->w, mpi->h,
                                      0, 0, mpi->w, mpi->h);
        if (!CHECK_VA_STATUS(p->ctx, "vaPutImage()"))
            return false;
    }

    return true;
}

// va_dst: copy destination, must be IMGFMT_VAAPI
// sw_src: copy source, must be a software surface
int va_surface_upload_image(struct mp_image *va_dst, struct mp_image *sw_src)
{
    struct va_surface *surface = va_surface_in_mp_image(va_dst);
    if (!surface)
        return -1;
    if (!va_surface_upload(surface, sw_src))
        return -1;
    return 0;
}

static struct mp_image *try_download(struct va_surface *surface,
                                     VAImageFormat *format,
                                     struct mp_image_pool *pool)
{
    VAStatus status;

    enum mp_imgfmt imgfmt = va_fourcc_to_imgfmt(format->fourcc);
    if (imgfmt == IMGFMT_NONE)
        return NULL;

    if (!va_surface_image_alloc(surface, format))
        return NULL;

    VAImage *image = &surface->p->image;

    if (!surface->p->is_derived) {
        status = vaGetImage(surface->p->display, surface->id, 0, 0,
                            surface->w, surface->h, image->image_id);
        if (status != VA_STATUS_SUCCESS)
            return NULL;
    }

    struct mp_image *dst = NULL;
    struct mp_image tmp;
    if (va_image_map(surface->p->ctx, image, &tmp)) {
        assert(tmp.imgfmt == imgfmt);
        dst = pool ? mp_image_pool_get(pool, imgfmt, tmp.w, tmp.h)
                    : mp_image_alloc(imgfmt, tmp.w, tmp.h);
        mp_image_copy(dst, &tmp);
        va_image_unmap(surface->p->ctx, image);
    }
    return dst;
}

// pool is optional (used for allocating returned images).
// Note: unlike va_surface_upload(), this will attempt to (re)create the
//       VAImage stored with the va_surface.
struct mp_image *va_surface_download(struct va_surface *surface,
                                     struct mp_image_pool *pool)
{
    struct mp_vaapi_ctx *ctx = surface->p->ctx;
    VAStatus status = vaSyncSurface(surface->p->display, surface->id);
    if (!CHECK_VA_STATUS(ctx, "vaSyncSurface()"))
        return NULL;

    VAImage *image = &surface->p->image;
    if (image->image_id != VA_INVALID_ID) {
        struct mp_image *mpi = try_download(surface, &image->format, pool);
        if (mpi)
            return mpi;
    }

    // We have no clue which format will work, so try them all.
    for (int i = 0; i < ctx->image_formats->num; i++) {
        VAImageFormat *format = &ctx->image_formats->entries[i];
        struct mp_image *mpi = try_download(surface, format, pool);
        if (mpi)
            return mpi;
    }

    MP_ERR(ctx, "failed to get surface data.\n");
    return NULL;
}

struct pool_alloc_ctx {
    struct mp_vaapi_ctx *vaapi;
    int rt_format;
};

static struct mp_image *alloc_pool(void *pctx, int fmt, int w, int h)
{
    struct pool_alloc_ctx *alloc_ctx = pctx;
    if (fmt != IMGFMT_VAAPI)
        return NULL;

    return alloc_surface(alloc_ctx->vaapi, alloc_ctx->rt_format, w, h);
}

// The allocator of the given image pool to allocate VAAPI surfaces, using
// the given rt_format.
void va_pool_set_allocator(struct mp_image_pool *pool, struct mp_vaapi_ctx *ctx,
                           int rt_format)
{
    struct pool_alloc_ctx *alloc_ctx = talloc_ptrtype(pool, alloc_ctx);
    *alloc_ctx = (struct pool_alloc_ctx){
        .vaapi = ctx,
        .rt_format = rt_format,
    };
    mp_image_pool_set_allocator(pool, alloc_pool, alloc_ctx);
    mp_image_pool_set_lru(pool);
}
