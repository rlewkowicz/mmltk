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


#ifndef AVCODEC_HWACCEL_INTERNAL_H
#define AVCODEC_HWACCEL_INTERNAL_H

#include <stdint.h>

#include "avcodec.h"
#include "libavutil/refstruct.h"

#define HWACCEL_CAP_ASYNC_SAFE      (1 << 0)
#define HWACCEL_CAP_THREAD_SAFE     (1 << 1)

typedef struct FFHWAccel {
    AVHWAccel p;

    int (*alloc_frame)(AVCodecContext *avctx, AVFrame *frame);

    int (*start_frame)(AVCodecContext *avctx, const AVBufferRef *buf_ref,
                       const uint8_t *buf, uint32_t buf_size);

    int (*decode_params)(AVCodecContext *avctx, int type, const uint8_t *buf, uint32_t buf_size);

    int (*decode_slice)(AVCodecContext *avctx, const uint8_t *buf, uint32_t buf_size);

    int (*end_frame)(AVCodecContext *avctx);

    int frame_priv_data_size;

    int priv_data_size;

    int caps_internal;

    int (*init)(AVCodecContext *avctx);

    int (*uninit)(AVCodecContext *avctx);

    int (*frame_params)(AVCodecContext *avctx, AVBufferRef *hw_frames_ctx);

    int (*update_thread_context)(AVCodecContext *dst, const AVCodecContext *src);

    void (*free_frame_priv)(AVRefStructOpaque hwctx, void *data);

    void (*flush)(AVCodecContext *avctx);
} FFHWAccel;

static inline const FFHWAccel *ffhwaccel(const AVHWAccel *codec)
{
    return (const FFHWAccel*)codec;
}

#define FF_HW_CALL(avctx, function, ...) \
        (ffhwaccel((avctx)->hwaccel)->function((avctx), __VA_ARGS__))

#define FF_HW_SIMPLE_CALL(avctx, function) \
        (ffhwaccel((avctx)->hwaccel)->function(avctx))

#define FF_HW_HAS_CB(avctx, function) \
        ((avctx)->hwaccel && ffhwaccel((avctx)->hwaccel)->function)

#endif /* AVCODEC_HWACCEL_INTERNAL */
