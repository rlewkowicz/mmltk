/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVUTIL_HWCONTEXT_INTERNAL_H
#define AVUTIL_HWCONTEXT_INTERNAL_H

#include <stddef.h>

#include "buffer.h"
#include "hwcontext.h"
#include "frame.h"
#include "pixfmt.h"

typedef struct HWContextType {
    enum AVHWDeviceType type;
    const char         *name;

    const enum AVPixelFormat *pix_fmts;

    size_t             device_hwctx_size;

    size_t             device_hwconfig_size;

    size_t             frames_hwctx_size;

    int              (*device_create)(AVHWDeviceContext *ctx, const char *device,
                                      AVDictionary *opts, int flags);
    int              (*device_derive)(AVHWDeviceContext *dst_ctx,
                                      AVHWDeviceContext *src_ctx,
                                      AVDictionary *opts, int flags);

    int              (*device_init)(AVHWDeviceContext *ctx);
    void             (*device_uninit)(AVHWDeviceContext *ctx);

    int              (*frames_get_constraints)(AVHWDeviceContext *ctx,
                                               const void *hwconfig,
                                               AVHWFramesConstraints *constraints);

    int              (*frames_init)(AVHWFramesContext *ctx);
    void             (*frames_uninit)(AVHWFramesContext *ctx);

    int              (*frames_get_buffer)(AVHWFramesContext *ctx, AVFrame *frame);
    int              (*transfer_get_formats)(AVHWFramesContext *ctx,
                                             enum AVHWFrameTransferDirection dir,
                                             enum AVPixelFormat **formats);
    int              (*transfer_data_to)(AVHWFramesContext *ctx, AVFrame *dst,
                                         const AVFrame *src);
    int              (*transfer_data_from)(AVHWFramesContext *ctx, AVFrame *dst,
                                           const AVFrame *src);

    int              (*map_to)(AVHWFramesContext *ctx, AVFrame *dst,
                               const AVFrame *src, int flags);
    int              (*map_from)(AVHWFramesContext *ctx, AVFrame *dst,
                                 const AVFrame *src, int flags);

    int              (*frames_derive_to)(AVHWFramesContext *dst_ctx,
                                         AVHWFramesContext *src_ctx, int flags);
    int              (*frames_derive_from)(AVHWFramesContext *dst_ctx,
                                           AVHWFramesContext *src_ctx, int flags);
} HWContextType;

typedef struct FFHWFramesContext {
    AVHWFramesContext p;

    const HWContextType *hw_type;

    AVBufferPool *pool_internal;

    AVBufferRef *source_frames;
    int source_allocation_map_flags;
} FFHWFramesContext;

static inline FFHWFramesContext *ffhwframesctx(AVHWFramesContext *ctx)
{
    return (FFHWFramesContext*)ctx;
}

typedef struct HWMapDescriptor {
    AVFrame *source;
    AVBufferRef *hw_frames_ctx;
    void (*unmap)(AVHWFramesContext *ctx,
                  struct HWMapDescriptor *hwmap);
    void          *priv;
} HWMapDescriptor;

int ff_hwframe_map_create(AVBufferRef *hwframe_ref,
                          AVFrame *dst, const AVFrame *src,
                          void (*unmap)(AVHWFramesContext *ctx,
                                        HWMapDescriptor *hwmap),
                          void *priv);

int ff_hwframe_map_replace(AVFrame *dst, const AVFrame *src);

extern const HWContextType ff_hwcontext_type_cuda;
extern const HWContextType ff_hwcontext_type_d3d11va;
extern const HWContextType ff_hwcontext_type_d3d12va;
extern const HWContextType ff_hwcontext_type_drm;
extern const HWContextType ff_hwcontext_type_dxva2;
extern const HWContextType ff_hwcontext_type_opencl;
extern const HWContextType ff_hwcontext_type_qsv;
extern const HWContextType ff_hwcontext_type_vaapi;
extern const HWContextType ff_hwcontext_type_vdpau;
extern const HWContextType ff_hwcontext_type_videotoolbox;
extern const HWContextType ff_hwcontext_type_mediacodec;
extern const HWContextType ff_hwcontext_type_vulkan;
extern const HWContextType ff_hwcontext_type_amf;
extern const HWContextType ff_hwcontext_type_oh;

#endif /* AVUTIL_HWCONTEXT_INTERNAL_H */
