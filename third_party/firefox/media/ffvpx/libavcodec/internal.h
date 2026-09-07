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


#ifndef AVCODEC_INTERNAL_H
#define AVCODEC_INTERNAL_H

#include <stdint.h>

#include "libavutil/channel_layout.h"
#include "avcodec.h"
#include "config.h"

#if CONFIG_LCMS2
# include "fflcms2.h"
#endif

#define FF_SANE_NB_CHANNELS 512U

#if HAVE_SIMD_ALIGN_64
#   define STRIDE_ALIGN 64 /* AVX-512 */
#elif HAVE_SIMD_ALIGN_32
#   define STRIDE_ALIGN 32
#elif HAVE_SIMD_ALIGN_16
#   define STRIDE_ALIGN 16
#else
#   define STRIDE_ALIGN 8
#endif

typedef struct AVCodecInternal {
    int is_copy;

    int is_frame_mt;

    int pad_samples;

    struct FramePool *pool;

    struct AVRefStructPool *progress_frame_pool;

    void *thread_ctx;

    AVPacket *in_pkt;
    struct AVBSFContext *bsf;

    AVPacket *last_pkt_props;

    uint8_t *byte_buffer;
    unsigned int byte_buffer_size;

    void *frame_thread_encoder;

    AVFrame *in_frame;

    AVFrame *recon_frame;

    int needs_close;

    int skip_samples;

    void *hwaccel_priv_data;

    /**
     * decoding: AVERROR_EOF has been returned from ff_decode_get_packet(); must
     *           not be used by decoders that use the decode() callback, as they
     *           do not call ff_decode_get_packet() directly.
     *
     * encoding: a flush frame has been submitted to avcodec_send_frame().
     */
    int draining;

    AVPacket *buffer_pkt;
    AVFrame *buffer_frame;
    int draining_done;

#if CONFIG_LCMS2
    FFIccContext icc; 
#endif

    int warned_on_failed_allocation_from_fixed_pool;
} AVCodecInternal;

int ff_match_2uint16(const uint16_t (*tab)[2], int size, int a, int b);

unsigned int ff_toupper4(unsigned int x);

int avpriv_h264_has_num_reorder_frames(AVCodecContext *avctx);

int avpriv_codec_get_cap_skip_frame_fill_param(const AVCodec *codec);

int ff_alloc_timecode_sei(const AVFrame *frame, AVRational rate, size_t prefix_len,
                     void **data, size_t *sei_size);

int64_t ff_guess_coded_bitrate(AVCodecContext *avctx);

#endif /* AVCODEC_INTERNAL_H */
