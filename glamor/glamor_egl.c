/*
 * Copyright © 2010 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including
 * the next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Zhigang Gong <zhigang.gong@linux.intel.com>
 *
 */
#include <dix-config.h>

#define GLAMOR_FOR_XORG
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <xf86.h>
#include <xf86Priv.h>
#include <xf86drm.h>
#define EGL_DISPLAY_NO_X_MESA

#include <gbm.h>
#include <drm_fourcc.h>

#include "dix/screen_hooks_priv.h"
#include "glamor/glamor_priv.h"
#include "os/bug_priv.h"

#include "glamor_egl.h"
#include "glamor_glx_provider.h"
#include "dri3.h"

struct glamor_egl_screen_private {
    EGLDisplay display;
    EGLContext context;
    char *device_path;

    int fd;
    struct gbm_device *gbm;
    int dmabuf_capable;
    Bool force_vendor; /* if GLVND vendor is forced from options */

    xf86FreeScreenProc *saved_free_screen;
};

int xf86GlamorEGLPrivateIndex = -1;


static struct glamor_egl_screen_private *
glamor_egl_get_screen_private(ScrnInfoPtr scrn)
{
    return (struct glamor_egl_screen_private *)
        scrn->privates[xf86GlamorEGLPrivateIndex].ptr;
}

static void
glamor_egl_make_current(struct glamor_context *glamor_ctx)
{
    /* There's only a single global dispatch table in Mesa.  EGL, GLX,
     * and AIGLX's direct dispatch table manipulation don't talk to
     * each other.  We need to set the context to NULL first to avoid
     * EGL's no-op context change fast path when switching back to
     * EGL.
     */
    eglMakeCurrent(glamor_ctx->display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (!eglMakeCurrent(glamor_ctx->display,
                        EGL_NO_SURFACE, EGL_NO_SURFACE,
                        glamor_ctx->ctx)) {
        FatalError("Failed to make EGL context current\n");
    }
}

static int
glamor_get_flink_name(int fd, int handle, int *name)
{
    struct drm_gem_flink flink;

    flink.handle = handle;
    if (ioctl(fd, DRM_IOCTL_GEM_FLINK, &flink) < 0) {

	/*
	 * Assume non-GEM kernels have names identical to the handle
	 */
	if (errno == ENODEV) {
	    *name = handle;
	    return TRUE;
	} else {
	    return FALSE;
	}
    }
    *name = flink.name;
    return TRUE;
}

static Bool
glamor_create_texture_from_image(ScreenPtr screen,
                                 EGLImageKHR image, GLuint * texture)
{
    struct glamor_screen_private *glamor_priv =
        glamor_get_screen_private(screen);

    glamor_make_current(glamor_priv);

    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glBindTexture(GL_TEXTURE_2D, 0);

    return TRUE;
}

struct gbm_device *
glamor_egl_get_gbm_device(ScreenPtr screen)
{
    struct glamor_egl_screen_private *glamor_egl =
        glamor_egl_get_screen_private(xf86ScreenToScrn(screen));
    return glamor_egl->gbm;
}

static void
glamor_egl_set_pixmap_image(PixmapPtr pixmap, EGLImageKHR image,
                            Bool used_modifiers)
{
    struct glamor_pixmap_private *pixmap_priv =
        glamor_get_pixmap_private(pixmap);
    EGLImageKHR old;

    BUG_RETURN(!pixmap_priv);

    old = pixmap_priv->image;
    if (old) {
        ScreenPtr                               screen = pixmap->drawable.pScreen;
        ScrnInfoPtr                             scrn = xf86ScreenToScrn(screen);
        struct glamor_egl_screen_private        *glamor_egl = glamor_egl_get_screen_private(scrn);

        eglDestroyImageKHR(glamor_egl->display, old);
    }
    pixmap_priv->image = image;
    pixmap_priv->used_modifiers = used_modifiers;
}

Bool
glamor_egl_create_textured_pixmap(PixmapPtr pixmap, int handle, int stride)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct glamor_egl_screen_private *glamor_egl =
        glamor_egl_get_screen_private(scrn);
    int ret, fd;

    /* GBM doesn't have an import path from handles, so we make a
     * dma-buf fd from it and then go through that.
     */
    ret = drmPrimeHandleToFD(glamor_egl->fd, handle, O_CLOEXEC, &fd);
    if (ret) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "Failed to make prime FD for handle: %d\n", errno);
        return FALSE;
    }

    if (!glamor_back_pixmap_from_fd(pixmap, fd,
                                    pixmap->drawable.width,
                                    pixmap->drawable.height,
                                    stride,
                                    pixmap->drawable.depth,
                                    pixmap->drawable.bitsPerPixel)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "Failed to make import prime FD as pixmap: %d\n", errno);
        close(fd);
        return FALSE;
    }

    close(fd);
    return TRUE;
}

Bool
glamor_egl_create_textured_pixmap_from_gbm_bo(PixmapPtr pixmap,
                                              struct gbm_bo *bo,
                                              Bool used_modifiers)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct glamor_screen_private *glamor_priv =
        glamor_get_screen_private(screen);
    struct glamor_egl_screen_private *glamor_egl;
    EGLImageKHR image;
    GLuint texture;
    Bool ret = FALSE;

    glamor_egl = glamor_egl_get_screen_private(scrn);
#ifdef GBM_BO_FD_FOR_PLANE
    uint64_t modifier = gbm_bo_get_modifier(bo);
    const int num_planes = gbm_bo_get_plane_count(bo);
    int fds[GBM_MAX_PLANES];
    int plane;
    int attr_num = 0;
    EGLint img_attrs[64] = {0};
    enum PlaneAttrs {
        PLANE_FD,
        PLANE_OFFSET,
        PLANE_PITCH,
        PLANE_MODIFIER_LO,
        PLANE_MODIFIER_HI,
        NUM_PLANE_ATTRS
    };
    static const EGLint planeAttrs[][NUM_PLANE_ATTRS] = {
        {
            EGL_DMA_BUF_PLANE0_FD_EXT,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        },
        {
            EGL_DMA_BUF_PLANE1_FD_EXT,
            EGL_DMA_BUF_PLANE1_OFFSET_EXT,
            EGL_DMA_BUF_PLANE1_PITCH_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        },
        {
            EGL_DMA_BUF_PLANE2_FD_EXT,
            EGL_DMA_BUF_PLANE2_OFFSET_EXT,
            EGL_DMA_BUF_PLANE2_PITCH_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        },
        {
            EGL_DMA_BUF_PLANE3_FD_EXT,
            EGL_DMA_BUF_PLANE3_OFFSET_EXT,
            EGL_DMA_BUF_PLANE3_PITCH_EXT,
            EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
        },
    };

    for (plane = 0; plane < num_planes; plane++) fds[plane] = -1;
#endif

    glamor_make_current(glamor_priv);

#ifdef GBM_BO_FD_FOR_PLANE
    if (glamor_egl->dmabuf_capable) {
#define ADD_ATTR(attrs, num, attr)                                      \
        do {                                                            \
            assert(((num) + 1) < (sizeof(attrs) / sizeof((attrs)[0]))); \
            (attrs)[(num)++] = (attr);                                  \
        } while (0)
        ADD_ATTR(img_attrs, attr_num, EGL_WIDTH);
        ADD_ATTR(img_attrs, attr_num, gbm_bo_get_width(bo));
        ADD_ATTR(img_attrs, attr_num, EGL_HEIGHT);
        ADD_ATTR(img_attrs, attr_num, gbm_bo_get_height(bo));
        ADD_ATTR(img_attrs, attr_num, EGL_LINUX_DRM_FOURCC_EXT);
        ADD_ATTR(img_attrs, attr_num, gbm_bo_get_format(bo));

        for (plane = 0; plane < num_planes; plane++) {
            fds[plane] = gbm_bo_get_fd_for_plane(bo, plane);
            ADD_ATTR(img_attrs, attr_num, planeAttrs[plane][PLANE_FD]);
            ADD_ATTR(img_attrs, attr_num, fds[plane]);
            ADD_ATTR(img_attrs, attr_num, planeAttrs[plane][PLANE_OFFSET]);
            ADD_ATTR(img_attrs, attr_num, gbm_bo_get_offset(bo, plane));
            ADD_ATTR(img_attrs, attr_num, planeAttrs[plane][PLANE_PITCH]);
            ADD_ATTR(img_attrs, attr_num, gbm_bo_get_stride_for_plane(bo, plane));
            ADD_ATTR(img_attrs, attr_num, planeAttrs[plane][PLANE_MODIFIER_LO]);
            ADD_ATTR(img_attrs, attr_num, (uint32_t)(modifier & 0xFFFFFFFFULL));
            ADD_ATTR(img_attrs, attr_num, planeAttrs[plane][PLANE_MODIFIER_HI]);
            ADD_ATTR(img_attrs, attr_num, (uint32_t)(modifier >> 32ULL));
        }
        ADD_ATTR(img_attrs, attr_num, EGL_NONE);
#undef ADD_ATTR

        image = eglCreateImageKHR(glamor_egl->display,
                                  EGL_NO_CONTEXT,
                                  EGL_LINUX_DMA_BUF_EXT,
                                  NULL,
                                  img_attrs);

        for (plane = 0; plane < num_planes; plane++) {
            close(fds[plane]);
            fds[plane] = -1;
        }
    }
    else
#endif
    {
        image = eglCreateImageKHR(glamor_egl->display,
                                  EGL_NO_CONTEXT,
                                  EGL_NATIVE_PIXMAP_KHR, bo, NULL);
    }

    if (image == EGL_NO_IMAGE_KHR) {
        glamor_set_pixmap_type(pixmap, GLAMOR_DRM_ONLY);
        goto done;
    }
    glamor_create_texture_from_image(screen, image, &texture);
    glamor_set_pixmap_type(pixmap, GLAMOR_TEXTURE_DRM);
    glamor_set_pixmap_texture(pixmap, texture);
    glamor_egl_set_pixmap_image(pixmap, image, used_modifiers);
    ret = TRUE;

 done:
    return ret;
}

static void
glamor_get_name_from_bo(int gbm_fd, struct gbm_bo *bo, int *name)
{
    union gbm_bo_handle handle;

    handle = gbm_bo_get_handle(bo);
    if (!glamor_get_flink_name(gbm_fd, handle.u32, name))
        *name = -1;
}

static Bool
glamor_make_pixmap_exportable(PixmapPtr pixmap, Bool modifiers_ok)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct glamor_egl_screen_private *glamor_egl =
        glamor_egl_get_screen_private(scrn);
    struct glamor_pixmap_private *pixmap_priv =
        glamor_get_pixmap_private(pixmap);
    unsigned width = pixmap->drawable.width;
    unsigned height = pixmap->drawable.height;
    uint32_t format;
    struct gbm_bo *bo = NULL;
    Bool used_modifiers = FALSE;
    PixmapPtr exported;
    GCPtr scratch_gc;

    BUG_RETURN_VAL(!pixmap_priv, FALSE);

    if (pixmap_priv->image &&
        (modifiers_ok || !pixmap_priv->used_modifiers))
        return TRUE;

    switch (pixmap->drawable.depth) {
    case 30:
        format = GBM_FORMAT_ARGB2101010;
        break;
    case 32:
    case 24:
        format = GBM_FORMAT_ARGB8888;
        break;
    case 16:
        format = GBM_FORMAT_RGB565;
        break;
    case 15:
        format = GBM_FORMAT_ARGB1555;
        break;
    case 8:
        format = GBM_FORMAT_R8;
        break;
    default:
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "Failed to make %d depth, %dbpp pixmap exportable\n",
                   pixmap->drawable.depth, pixmap->drawable.bitsPerPixel);
        return FALSE;
    }

#ifdef GBM_BO_WITH_MODIFIERS
    if (modifiers_ok && glamor_egl->dmabuf_capable) {
        uint32_t num_modifiers;
        uint64_t *modifiers = NULL;

        glamor_get_modifiers(screen, format, &num_modifiers, &modifiers);

        if (num_modifiers > 0) {
#ifdef GBM_BO_WITH_MODIFIERS2
            /* TODO: Is scanout ever used? If so, where? */
            bo = gbm_bo_create_with_modifiers2(glamor_egl->gbm, width, height,
                                               format, modifiers, num_modifiers,
                                               GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
            if (!bo) {
                /* something failed, try again without GBM_BO_USE_SCANOUT */
                /* maybe scanout does work, but modifiers aren't supported */
                /* we handle this case on the fallback path */
                bo = gbm_bo_create_with_modifiers2(glamor_egl->gbm, width, height,
                                                   format, modifiers, num_modifiers,
                                                   GBM_BO_USE_RENDERING);
#if 0
                if (bo) {
                    /* TODO: scanout failed, but regular buffer succeeded, maybe log something? */
                }
#endif
            }
#else
            bo = gbm_bo_create_with_modifiers(glamor_egl->gbm, width, height,
                                              format, modifiers, num_modifiers);
#endif
        }
        if (bo)
            used_modifiers = TRUE;
        free(modifiers);
    }
#endif

    if (!bo)
    {
        /* TODO: Is scanout ever used? If so, where? */
        bo = gbm_bo_create(glamor_egl->gbm, width, height, format,
#ifdef GLAMOR_HAS_GBM_LINEAR
                (pixmap->usage_hint == CREATE_PIXMAP_USAGE_SHARED ?
                 GBM_BO_USE_LINEAR : 0) |
#endif
                GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        if (!bo) {
            /* something failed, try again without GBM_BO_USE_SCANOUT */
            bo = gbm_bo_create(glamor_egl->gbm, width, height, format,
#ifdef GLAMOR_HAS_GBM_LINEAR
                    (pixmap->usage_hint == CREATE_PIXMAP_USAGE_SHARED ?
                     GBM_BO_USE_LINEAR : 0) |
                     GBM_BO_USE_RENDERING);
#endif
#if 0
            if (bo) {
                /* TODO: scanout failed, but regular buffer succeeded, maybe log something? */
            }
#endif
        }
    }

    if (!bo) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "Failed to make %dx%dx%dbpp GBM bo\n",
                   width, height, pixmap->drawable.bitsPerPixel);
        return FALSE;
    }

    exported = screen->CreatePixmap(screen, 0, 0, pixmap->drawable.depth, 0);
    screen->ModifyPixmapHeader(exported, width, height, 0, 0,
                               gbm_bo_get_stride(bo), NULL);
    if (!glamor_egl_create_textured_pixmap_from_gbm_bo(exported, bo,
                                                       used_modifiers)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "Failed to make %dx%dx%dbpp pixmap from GBM bo\n",
                   width, height, pixmap->drawable.bitsPerPixel);
        dixDestroyPixmap(exported, 0);
        gbm_bo_destroy(bo);
        return FALSE;
    }
    gbm_bo_destroy(bo);

    scratch_gc = GetScratchGC(pixmap->drawable.depth, screen);
    ValidateGC(&pixmap->drawable, scratch_gc);
    (void) scratch_gc->ops->CopyArea(&pixmap->drawable, &exported->drawable,
                              scratch_gc,
                              0, 0, width, height, 0, 0);
    FreeScratchGC(scratch_gc);

    /* Now, swap the tex/gbm/EGLImage/etc. of the exported pixmap into
     * the original pixmap struct.
     */
    glamor_egl_exchange_buffers(pixmap, exported);

    /* Swap the devKind into the original pixmap, reflecting the bo's stride */
    screen->ModifyPixmapHeader(pixmap, 0, 0, 0, 0, exported->devKind, NULL);

    dixDestroyPixmap(exported, 0);

    return TRUE;
}

static struct gbm_bo *
glamor_gbm_bo_from_pixmap_internal(ScreenPtr screen, PixmapPtr pixmap)
{
    struct glamor_egl_screen_private *glamor_egl =
        glamor_egl_get_screen_private(xf86ScreenToScrn(screen));
    struct glamor_pixmap_private *pixmap_priv =
        glamor_get_pixmap_private(pixmap);

    BUG_RETURN_VAL(!pixmap_priv, NULL);

    if (!pixmap_priv->image)
        return NULL;

    return gbm_bo_import(glamor_egl->gbm, GBM_BO_IMPORT_EGL_IMAGE,
                         pixmap_priv->image, 0);
}

struct gbm_bo *
glamor_gbm_bo_from_pixmap(ScreenPtr screen, PixmapPtr pixmap)
{
    if (!glamor_make_pixmap_exportable(pixmap, TRUE))
        return NULL;

    return glamor_gbm_bo_from_pixmap_internal(screen, pixmap);
}

int
glamor_egl_fds_from_pixmap(ScreenPtr screen, PixmapPtr pixmap, int *fds,
                           uint32_t *strides, uint32_t *offsets,
                           uint64_t *modifier)
{
#ifdef GLAMOR_HAS_GBM
    struct gbm_bo *bo;
    int num_fds;
#ifdef GBM_BO_WITH_MODIFIERS
#ifndef GBM_BO_FD_FOR_PLANE
    int32_t first_handle;
#endif
    int i;
#endif

    if (!glamor_make_pixmap_exportable(pixmap, TRUE))
        return 0;

    bo = glamor_gbm_bo_from_pixmap_internal(screen, pixmap);
    if (!bo)
        return 0;

#ifdef GBM_BO_WITH_MODIFIERS
    num_fds = gbm_bo_get_plane_count(bo);
    for (i = 0; i < num_fds; i++) {
#ifdef GBM_BO_FD_FOR_PLANE
        fds[i] = gbm_bo_get_fd_for_plane(bo, i);
#else
        union gbm_bo_handle plane_handle = gbm_bo_get_handle_for_plane(bo, i);

        if (i == 0)
            first_handle = plane_handle.s32;

        /* If all planes point to the same object as the first plane, i.e. they
         * all have the same handle, we can fall back to the non-planar
         * gbm_bo_get_fd without losing information. If they point to different
         * objects we are out of luck and need to give up.
         */
	if (first_handle == plane_handle.s32)
            fds[i] = gbm_bo_get_fd(bo);
        else
            fds[i] = -1;
#endif
        if (fds[i] == -1) {
            while (--i >= 0)
                close(fds[i]);
            return 0;
        }
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
    }
    *modifier = gbm_bo_get_modifier(bo);
#else
    num_fds = 1;
    fds[0] = gbm_bo_get_fd(bo);
    if (fds[0] == -1)
        return 0;
    strides[0] = gbm_bo_get_stride(bo);
    offsets[0] = 0;
    *modifier = DRM_FORMAT_MOD_INVALID;
#endif

    gbm_bo_destroy(bo);
    return num_fds;
#else
    return 0;
#endif
}

int
glamor_egl_fd_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                          CARD16 *stride, CARD32 *size)
{
#ifdef GLAMOR_HAS_GBM
    struct gbm_bo *bo;
    int fd;

    if (!glamor_make_pixmap_exportable(pixmap, FALSE))
        return -1;

    bo = glamor_gbm_bo_from_pixmap_internal(screen, pixmap);
    if (!bo)
        return -1;

    fd = gbm_bo_get_fd(bo);
    *stride = gbm_bo_get_stride(bo);
    *size = *stride * gbm_bo_get_height(bo);
    gbm_bo_destroy(bo);

    return fd;
#else
    return -1;
#endif
}

int
glamor_egl_fd_name_from_pixmap(ScreenPtr screen,
                               PixmapPtr pixmap,
                               CARD16 *stride, CARD32 *size)
{
    struct glamor_egl_screen_private *glamor_egl;
    struct gbm_bo *bo;
    int fd = -1;

    glamor_egl = glamor_egl_get_screen_private(xf86ScreenToScrn(screen));

    if (!glamor_make_pixmap_exportable(pixmap, FALSE))
        goto failure;

    bo = glamor_gbm_bo_from_pixmap_internal(screen, pixmap);
    if (!bo)
        goto failure;

    pixmap->devKind = gbm_bo_get_stride(bo);

    glamor_get_name_from_bo(glamor_egl->fd, bo, &fd);
    *stride = pixmap->devKind;
    *size = pixmap->devKind * gbm_bo_get_height(bo);

    gbm_bo_destroy(bo);
 failure:
    return fd;
}

static bool
gbm_format_for_depth(CARD8 depth, uint32_t *format)
{
    switch (depth) {
    case 15:
        *format = GBM_FORMAT_ARGB1555;
        return true;
    case 16:
        *format = GBM_FORMAT_RGB565;
        return true;
    case 24:
        *format = GBM_FORMAT_XRGB8888;
        return true;
    case 30:
        *format = GBM_FORMAT_ARGB2101010;
        return true;
    case 32:
        *format = GBM_FORMAT_ARGB8888;
        return true;
    default:
        ErrorF("unexpected depth: %d\n", depth);
        return false;
    }
}

Bool
glamor_back_pixmap_from_fd(PixmapPtr pixmap,
                           int fd,
                           CARD16 width,
                           CARD16 height,
                           CARD16 stride, CARD8 depth, CARD8 bpp)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct glamor_egl_screen_private *glamor_egl;
    struct gbm_bo *bo;
    struct gbm_import_fd_data import_data = { 0 };
    Bool ret;

    glamor_egl = glamor_egl_get_screen_private(scrn);

    if (!gbm_format_for_depth(depth, &import_data.format) ||
        width == 0 || height == 0)
        return FALSE;

    import_data.fd = fd;
    import_data.width = width;
    import_data.height = height;
    import_data.stride = stride;
    bo = gbm_bo_import(glamor_egl->gbm, GBM_BO_IMPORT_FD, &import_data, 0);
    if (!bo)
        return FALSE;

    screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, stride, NULL);

    ret = glamor_egl_create_textured_pixmap_from_gbm_bo(pixmap, bo, FALSE);
    gbm_bo_destroy(bo);
    return ret;
}

PixmapPtr
glamor_pixmap_from_fds(ScreenPtr screen,
                       CARD8 num_fds, const int *fds,
                       CARD16 width, CARD16 height,
                       const CARD32 *strides, const CARD32 *offsets,
                       CARD8 depth, CARD8 bpp,
                       uint64_t modifier)
{
    PixmapPtr pixmap;
    struct glamor_egl_screen_private *glamor_egl;
    Bool ret = FALSE;
    int i;

    glamor_egl = glamor_egl_get_screen_private(xf86ScreenToScrn(screen));

    pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);

#ifdef GBM_BO_WITH_MODIFIERS
    if (glamor_egl->dmabuf_capable && modifier != DRM_FORMAT_MOD_INVALID) {
        struct gbm_import_fd_modifier_data import_data = { 0 };
        struct gbm_bo *bo;

        if (!gbm_format_for_depth(depth, &import_data.format) ||
            width == 0 || height == 0)
            goto error;

        import_data.width = width;
        import_data.height = height;
        import_data.num_fds = num_fds;
        import_data.modifier = modifier;
        for (i = 0; i < num_fds; i++) {
            import_data.fds[i] = fds[i];
            import_data.strides[i] = strides[i];
            import_data.offsets[i] = offsets[i];
        }
        bo = gbm_bo_import(glamor_egl->gbm, GBM_BO_IMPORT_FD_MODIFIER, &import_data, 0);
        if (bo) {
            screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, strides[0], NULL);
            ret = glamor_egl_create_textured_pixmap_from_gbm_bo(pixmap, bo, TRUE);
            gbm_bo_destroy(bo);
        }
    } else
#endif
    {
        if (num_fds == 1) {
            ret = glamor_back_pixmap_from_fd(pixmap, fds[0], width, height,
                                             strides[0], depth, bpp);
        }
    }

error:
    if (ret == FALSE) {
        dixDestroyPixmap(pixmap, 0);
        return NULL;
    }
    return pixmap;
}

PixmapPtr
glamor_pixmap_from_fd(ScreenPtr screen,
                      int fd,
                      CARD16 width,
                      CARD16 height,
                      CARD16 stride, CARD8 depth, CARD8 bpp)
{
    PixmapPtr pixmap;
    Bool ret;

    pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);

    ret = glamor_back_pixmap_from_fd(pixmap, fd, width, height,
                                     stride, depth, bpp);

    if (ret == FALSE) {
        dixDestroyPixmap(pixmap, 0);
        return NULL;
    }
    return pixmap;
}

Bool
glamor_get_formats(ScreenPtr screen,
                   CARD32 *num_formats, CARD32 **formats)
{
#ifdef GLAMOR_HAS_EGL_QUERY_DMABUF
    struct glamor_egl_screen_private *glamor_egl;
    EGLint num;

    /* Explicitly zero the count as the caller may ignore the return value */
    *num_formats = 0;

    glamor_egl = glamor_egl_get_screen_private(xf86ScreenToScrn(screen));

    if (!glamor_egl->dmabuf_capable)
        return TRUE;

    if (!eglQueryDmaBufFormatsEXT(glamor_egl->display, 0, NULL, &num))
        return FALSE;

    if (num == 0)
        return TRUE;

    *formats = calloc(num, sizeof(CARD32));
    if (*formats == NULL)
        return FALSE;

    if (!eglQueryDmaBufFormatsEXT(glamor_egl->display, num,
                                  (EGLint *) *formats, &num)) {
        free(*formats);
        return FALSE;
    }

    *num_formats = num;
    return TRUE;
#else
    *num_formats = 0;
    return TRUE;
#endif
}

Bool
glamor_get_modifiers(ScreenPtr screen, uint32_t format,
                     uint32_t *num_modifiers, uint64_t **modifiers)
{
#ifdef GLAMOR_HAS_EGL_QUERY_DMABUF
    struct glamor_egl_screen_private *glamor_egl;
    EGLint num;
#endif

    /* Explicitly zero the count and modifiers as the caller may ignore the return value */
    *num_modifiers = 0;
    *modifiers = NULL;
#ifdef GLAMOR_HAS_EGL_QUERY_DMABUF
    glamor_egl = glamor_egl_get_screen_private(xf86ScreenToScrn(screen));

    if (!glamor_egl->dmabuf_capable)
        return FALSE;

    if (!eglQueryDmaBufModifiersEXT(glamor_egl->display, format, 0, NULL,
                                    NULL, &num))
        return FALSE;

    if (num == 0)
        return TRUE;

    *modifiers = calloc(num, sizeof(uint64_t));
    if (*modifiers == NULL)
        return FALSE;

    if (!eglQueryDmaBufModifiersEXT(glamor_egl->display, format, num,
                                    (EGLuint64KHR *) *modifiers, NULL, &num)) {
        free(*modifiers);
        return FALSE;
    }

    *num_modifiers = num;
#endif
    return TRUE;
}

const char *
glamor_egl_get_driver_name(ScreenPtr screen)
{
#ifdef GLAMOR_HAS_EGL_QUERY_DRIVER
    struct glamor_egl_screen_private *glamor_egl;

    glamor_egl = glamor_egl_get_screen_private(xf86ScreenToScrn(screen));

    if (epoxy_has_egl_extension(glamor_egl->display, "EGL_MESA_query_driver"))
        return eglGetDisplayDriverName(glamor_egl->display);
#endif

    return NULL;
}

static void glamor_egl_pixmap_destroy(CallbackListPtr *pcbl, ScreenPtr pScreen, PixmapPtr pixmap)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct glamor_egl_screen_private *glamor_egl =
        glamor_egl_get_screen_private(scrn);

    struct glamor_pixmap_private *pixmap_priv =
        glamor_get_pixmap_private(pixmap);

    BUG_RETURN(!pixmap_priv);

    if (pixmap_priv->image)
        eglDestroyImageKHR(glamor_egl->display, pixmap_priv->image);
}

void
glamor_egl_exchange_buffers(PixmapPtr front, PixmapPtr back)
{
    EGLImageKHR temp_img;
    Bool temp_mod;
    struct glamor_pixmap_private *front_priv =
        glamor_get_pixmap_private(front);
    struct glamor_pixmap_private *back_priv =
        glamor_get_pixmap_private(back);

    glamor_pixmap_exchange_fbos(front, back);

    temp_img = back_priv->image;
    temp_mod = back_priv->used_modifiers;
    BUG_RETURN(!back_priv);
    back_priv->image = front_priv->image;
    back_priv->used_modifiers = front_priv->used_modifiers;
    BUG_RETURN(!front_priv);
    front_priv->image = temp_img;
    front_priv->used_modifiers = temp_mod;

    glamor_set_pixmap_type(front, GLAMOR_TEXTURE_DRM);
    glamor_set_pixmap_type(back, GLAMOR_TEXTURE_DRM);
}

static void glamor_egl_close_screen(CallbackListPtr *pcbl, ScreenPtr screen, void *unused)
{
    ScrnInfoPtr scrn;
    struct glamor_egl_screen_private *glamor_egl;
    struct glamor_pixmap_private *pixmap_priv;
    PixmapPtr screen_pixmap;

    scrn = xf86ScreenToScrn(screen);
    glamor_egl = glamor_egl_get_screen_private(scrn);
    screen_pixmap = screen->GetScreenPixmap(screen);

    pixmap_priv = glamor_get_pixmap_private(screen_pixmap);
    BUG_RETURN(!pixmap_priv);

    eglDestroyImageKHR(glamor_egl->display, pixmap_priv->image);
    pixmap_priv->image = NULL;

    dixScreenUnhookClose(screen, glamor_egl_close_screen);
    dixScreenUnhookPixmapDestroy(screen, glamor_egl_pixmap_destroy);
}

#ifdef DRI3
static int
glamor_dri3_open_client(ClientPtr client,
                        ScreenPtr screen,
                        RRProviderPtr provider,
                        int *fdp)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct glamor_egl_screen_private *glamor_egl =
        glamor_egl_get_screen_private(scrn);
    int fd;
    drm_magic_t magic;

    fd = open(glamor_egl->device_path, O_RDWR|O_CLOEXEC);
    if (fd < 0)
        return BadAlloc;

    /* Before FD passing in the X protocol with DRI3 (and increased
     * security of rendering with per-process address spaces on the
     * GPU), the kernel had to come up with a way to have the server
     * decide which clients got to access the GPU, which was done by
     * each client getting a unique (magic) number from the kernel,
     * passing it to the server, and the server then telling the
     * kernel which clients were authenticated for using the device.
     *
     * Now that we have FD passing, the server can just set up the
     * authentication on its own and hand the prepared FD off to the
     * client.
     */
    if (drmGetMagic(fd, &magic) < 0) {
        if (errno == EACCES) {
            /* Assume that we're on a render node, and the fd is
             * already as authenticated as it should be.
             */
            *fdp = fd;
            return Success;
        } else {
            close(fd);
            return BadMatch;
        }
    }

    if (drmAuthMagic(glamor_egl->fd, magic) < 0) {
        close(fd);
        return BadMatch;
    }

    *fdp = fd;
    return Success;
}

static const dri3_screen_info_rec glamor_dri3_info = {
    .version = 2,
    .open_client = glamor_dri3_open_client,
    .pixmap_from_fds = glamor_pixmap_from_fds,
    .fd_from_pixmap = glamor_egl_fd_from_pixmap,
    .fds_from_pixmap = glamor_egl_fds_from_pixmap,
    .get_formats = glamor_get_formats,
    .get_modifiers = glamor_get_modifiers,
    .get_drawable_modifiers = glamor_get_drawable_modifiers,
};
#endif /* DRI3 */

void
glamor_egl_screen_init(ScreenPtr screen, struct glamor_context *glamor_ctx)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct glamor_egl_screen_private *glamor_egl =
        glamor_egl_get_screen_private(scrn);
#ifdef DRI3
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);
#endif
#ifdef GLXEXT
    static Bool vendor_initialized = FALSE;
#endif
    const char *gbm_backend_name;

    dixScreenHookClose(screen, glamor_egl_close_screen);
    dixScreenHookPixmapDestroy(screen, glamor_egl_pixmap_destroy);

    glamor_ctx->ctx = glamor_egl->context;
    glamor_ctx->display = glamor_egl->display;

    glamor_ctx->make_current = glamor_egl_make_current;

    /* Use dynamic logic only if vendor is not forced via xorg.conf */
    if (!glamor_egl->force_vendor) {
        gbm_backend_name = gbm_device_get_backend_name(glamor_egl->gbm);
        /* Mesa uses "drm" as backend name, in that case, just do nothing */
        if (gbm_backend_name && strcmp(gbm_backend_name, "drm") != 0)
            glamor_set_glvnd_vendor(screen, gbm_backend_name);
    }
#ifdef DRI3
    /* Tell the core that we have the interfaces for import/export
     * of pixmaps.
     */
    glamor_enable_dri3(screen);

    /* If the driver wants to do its own auth dance (e.g. Xwayland
     * on pre-3.15 kernels that don't have render nodes and thus
     * has the wayland compositor as a master), then it needs us
     * to stay out of the way and let it init DRI3 on its own.
     */
    if (!(glamor_priv->flags & GLAMOR_NO_DRI3)) {
        /* To do DRI3 device FD generation, we need to open a new fd
         * to the same device we were handed in originally.
         */
        glamor_egl->device_path = drmGetRenderDeviceNameFromFd(glamor_egl->fd);
        if (!glamor_egl->device_path)
            glamor_egl->device_path = drmGetDeviceNameFromFd2(glamor_egl->fd);

        if (!dri3_screen_init(screen, &glamor_dri3_info)) {
            xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                       "Failed to initialize DRI3.\n");
        }
    }
#endif
#ifdef GLXEXT
    if (!vendor_initialized) {
        GlxPushProvider(&glamor_provider);
        xorgGlxCreateVendor();
        vendor_initialized = TRUE;
    }
#endif
}

static void glamor_egl_cleanup(struct glamor_egl_screen_private *glamor_egl)
{
    if (glamor_egl->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(glamor_egl->display,
                       EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        /*
         * Force the next glamor_make_current call to update the context
         * (on hot unplug another GPU may still be using glamor)
         */
        lastGLContext = NULL;
        eglTerminate(glamor_egl->display);
    }
    if (glamor_egl->gbm)
        gbm_device_destroy(glamor_egl->gbm);
    free(glamor_egl->device_path);
    free(glamor_egl);
}

static void
glamor_egl_free_screen(ScrnInfoPtr scrn)
{
    struct glamor_egl_screen_private *glamor_egl;

    glamor_egl = glamor_egl_get_screen_private(scrn);
    if (glamor_egl != NULL) {
        scrn->FreeScreen = glamor_egl->saved_free_screen;
        glamor_egl_cleanup(glamor_egl);
        scrn->FreeScreen(scrn);
    }
}

static Bool
glamor_egl_try_big_gl_api(ScrnInfoPtr scrn)
{
    struct glamor_egl_screen_private *glamor_egl =
        glamor_egl_get_screen_private(scrn);

    if (eglBindAPI(EGL_OPENGL_API)) {
        static const EGLint config_attribs_core[] = {
            EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
            EGL_CONTEXT_MAJOR_VERSION_KHR,
            GLAMOR_GL_CORE_VER_MAJOR,
            EGL_CONTEXT_MINOR_VERSION_KHR,
            GLAMOR_GL_CORE_VER_MINOR,
            EGL_NONE
        };
        static const EGLint config_attribs[] = {
            EGL_NONE
        };

        glamor_egl->context = eglCreateContext(glamor_egl->display,
                                               EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
                                               config_attribs_core);

        if (glamor_egl->context == EGL_NO_CONTEXT)
            glamor_egl->context = eglCreateContext(glamor_egl->display,
                                                   EGL_NO_CONFIG_KHR,
                                                   EGL_NO_CONTEXT,
                                                   config_attribs);
    }

    if (glamor_egl->context != EGL_NO_CONTEXT) {
        if (!eglMakeCurrent(glamor_egl->display,
                            EGL_NO_SURFACE, EGL_NO_SURFACE, glamor_egl->context)) {
            xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                       "Failed to make GL context current\n");
            return FALSE;
        }

        if (epoxy_gl_version() < 21) {
            xf86DrvMsg(scrn->scrnIndex, X_INFO,
                       "glamor: Ignoring GL < 2.1, falling back to GLES.\n");
            eglDestroyContext(glamor_egl->display, glamor_egl->context);
            glamor_egl->context = EGL_NO_CONTEXT;
        }
        xf86DrvMsg(scrn->scrnIndex, X_INFO,
            "glamor: Using OpenGL %d.%d context.\n",
            epoxy_gl_version() / 10,
            epoxy_gl_version() % 10);
    }
    return TRUE;
}

static Bool
glamor_egl_try_gles_api(ScrnInfoPtr scrn)
{
    struct glamor_egl_screen_private *glamor_egl =
        glamor_egl_get_screen_private(scrn);
        
    static const EGLint config_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                    "glamor: Failed to bind GLES API.\n");
        return FALSE;
    }

    glamor_egl->context = eglCreateContext(glamor_egl->display,
                                            EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
                                            config_attribs);

    if (glamor_egl->context != EGL_NO_CONTEXT) {
        if (!eglMakeCurrent(glamor_egl->display,
                            EGL_NO_SURFACE, EGL_NO_SURFACE, glamor_egl->context)) {
            xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                       "Failed to make GLES context current\n");
            return FALSE;
        }
        xf86DrvMsg(scrn->scrnIndex, X_INFO,
                "glamor: Using OpenGL ES %d.%d context.\n",
                epoxy_gl_version() / 10,
                epoxy_gl_version() % 10);
    }
    return TRUE;
}

enum {
    GLAMOREGLOPT_RENDERING_API,
    GLAMOREGLOPT_VENDOR_LIBRARY
};

static const OptionInfoRec GlamorEGLOptions[] = {
    { GLAMOREGLOPT_RENDERING_API, "RenderingAPI", OPTV_STRING, {0}, FALSE },
    { GLAMOREGLOPT_VENDOR_LIBRARY, "GlxVendorLibrary", OPTV_STRING, {0}, FALSE },
    { -1, NULL, OPTV_NONE, {0}, FALSE },
};

Bool
glamor_egl_init(ScrnInfoPtr scrn, int fd)
{
    struct glamor_egl_screen_private *glamor_egl;
    const GLubyte *renderer;
    OptionInfoPtr options;
    const char *api = NULL;
    Bool es_allowed = TRUE;
    Bool force_es = FALSE;
    const char *glvnd_vendor = NULL;

    glamor_egl = calloc(1, sizeof(*glamor_egl));
    if (glamor_egl == NULL)
        return FALSE;
    if (xf86GlamorEGLPrivateIndex == -1)
        xf86GlamorEGLPrivateIndex = xf86AllocateScrnInfoPrivateIndex();

    options = XNFalloc(sizeof(GlamorEGLOptions));
    memcpy(options, GlamorEGLOptions, sizeof(GlamorEGLOptions));
    xf86ProcessOptions(scrn->scrnIndex, scrn->options, options);
    glvnd_vendor = xf86GetOptValString(options, GLAMOREGLOPT_VENDOR_LIBRARY);
    if (glvnd_vendor) {
        glamor_set_glvnd_vendor(xf86ScrnToScreen(scrn), glvnd_vendor);
        glamor_egl->force_vendor = TRUE;
    }
    api = xf86GetOptValString(options, GLAMOREGLOPT_RENDERING_API);
    if (api && !strncasecmp(api, "es", 2))
        force_es = TRUE;
    else if (api && !strncasecmp(api, "gl", 2))
        es_allowed = FALSE;
    free(options);

    scrn->privates[xf86GlamorEGLPrivateIndex].ptr = glamor_egl;
    glamor_egl->fd = fd;
    glamor_egl->gbm = gbm_create_device(glamor_egl->fd);
    if (glamor_egl->gbm == NULL) {
        ErrorF("couldn't get display device\n");
        goto error;
    }

    glamor_egl->display = glamor_egl_get_display(EGL_PLATFORM_GBM_MESA,
                                                 glamor_egl->gbm);
    if (!glamor_egl->display) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR, "eglGetDisplay() failed\n");
        goto error;
    }

    if (!eglInitialize(glamor_egl->display, NULL, NULL)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR, "eglInitialize() failed\n");
        glamor_egl->display = EGL_NO_DISPLAY;
        goto error;
    }

#define GLAMOR_CHECK_EGL_EXTENSION(EXT)  \
	if (!epoxy_has_egl_extension(glamor_egl->display, "EGL_" #EXT)) {  \
		ErrorF("EGL_" #EXT " required.\n");  \
		goto error;  \
	}

#define GLAMOR_CHECK_EGL_EXTENSIONS(EXT1, EXT2)	 \
	if (!epoxy_has_egl_extension(glamor_egl->display, "EGL_" #EXT1) &&  \
	    !epoxy_has_egl_extension(glamor_egl->display, "EGL_" #EXT2)) {  \
		ErrorF("EGL_" #EXT1 " or EGL_" #EXT2 " required.\n");  \
		goto error;  \
	}

    GLAMOR_CHECK_EGL_EXTENSION(KHR_surfaceless_context);
    GLAMOR_CHECK_EGL_EXTENSION(KHR_no_config_context);

    if (!force_es) {
        if(!glamor_egl_try_big_gl_api(scrn))
            goto error;
    }

    if (glamor_egl->context == EGL_NO_CONTEXT && es_allowed) {
        if(!glamor_egl_try_gles_api(scrn))
            goto error;
    }

    if (glamor_egl->context == EGL_NO_CONTEXT) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                    "glamor: Failed to create GL or GLES2 contexts\n");
        goto error;
    }

    renderer = glGetString(GL_RENDERER);
    if (!renderer) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "glGetString() returned NULL, your GL is broken\n");
        goto error;
    }
    if (strstr((const char *)renderer, "softpipe")) {
        xf86DrvMsg(scrn->scrnIndex, X_INFO,
                   "Refusing to try glamor on softpipe\n");
        goto error;
    }
    if (!strncmp("llvmpipe", (const char *)renderer, strlen("llvmpipe"))) {
        if (scrn->confScreen->num_gpu_devices)
            xf86DrvMsg(scrn->scrnIndex, X_INFO,
                       "Allowing glamor on llvmpipe for PRIME\n");
        else {
            xf86DrvMsg(scrn->scrnIndex, X_INFO,
                       "Refusing to try glamor on llvmpipe\n");
            goto error;
        }
    }

    /*
     * Force the next glamor_make_current call to set the right context
     * (in case of multiple GPUs using glamor)
     */
    lastGLContext = NULL;

    if (!epoxy_has_gl_extension("GL_OES_EGL_image")) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "glamor acceleration requires GL_OES_EGL_image\n");
        goto error;
    }

    xf86DrvMsg(scrn->scrnIndex, X_INFO, "glamor X acceleration enabled on %s\n",
               renderer);

#ifdef GBM_BO_WITH_MODIFIERS
    if (epoxy_has_egl_extension(glamor_egl->display,
                                "EGL_EXT_image_dma_buf_import") &&
        epoxy_has_egl_extension(glamor_egl->display,
                                "EGL_EXT_image_dma_buf_import_modifiers")) {
        if (xf86Info.debug != NULL)
            glamor_egl->dmabuf_capable = !!strstr(xf86Info.debug,
                                                  "dmabuf_capable");
        else if (strstr((const char *)renderer, "Intel"))
            glamor_egl->dmabuf_capable = TRUE;
        else if (strstr((const char *)renderer, "zink"))
            glamor_egl->dmabuf_capable = TRUE;
        else if (strstr((const char *)renderer, "NVIDIA"))
            glamor_egl->dmabuf_capable = TRUE;
        else
            glamor_egl->dmabuf_capable = FALSE;
    }
#endif

    glamor_egl->saved_free_screen = scrn->FreeScreen;
    scrn->FreeScreen = glamor_egl_free_screen;
    return TRUE;

error:
    glamor_egl_cleanup(glamor_egl);
    return FALSE;
}

/** Stub to retain compatibility with pre-server-1.16 ABI. */
Bool
glamor_egl_init_textured_pixmap(ScreenPtr screen)
{
    return TRUE;
}
