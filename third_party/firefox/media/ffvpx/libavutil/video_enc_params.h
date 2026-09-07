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

#ifndef AVUTIL_VIDEO_ENC_PARAMS_H
#define AVUTIL_VIDEO_ENC_PARAMS_H

#include <stddef.h>
#include <stdint.h>

#include "libavutil/avassert.h"
#include "libavutil/frame.h"

enum AVVideoEncParamsType {
    AV_VIDEO_ENC_PARAMS_NONE = -1,
    AV_VIDEO_ENC_PARAMS_VP9,

    AV_VIDEO_ENC_PARAMS_H264,

    AV_VIDEO_ENC_PARAMS_MPEG2,
};

typedef struct AVVideoEncParams {
    unsigned int nb_blocks;
    size_t blocks_offset;
    size_t block_size;

    enum AVVideoEncParamsType type;

    int32_t qp;

    int32_t delta_qp[4][2];
} AVVideoEncParams;

typedef struct AVVideoBlockParams {
    int src_x, src_y;
    int w, h;

    int32_t delta_qp;
} AVVideoBlockParams;

static av_always_inline AVVideoBlockParams*
av_video_enc_params_block(AVVideoEncParams *par, unsigned int idx)
{
    av_assert0(idx < par->nb_blocks);
    return (AVVideoBlockParams *)((uint8_t *)par + par->blocks_offset +
                                  idx * par->block_size);
}

AVVideoEncParams *av_video_enc_params_alloc(enum AVVideoEncParamsType type,
                                            unsigned int nb_blocks, size_t *out_size);

AVVideoEncParams*
av_video_enc_params_create_side_data(AVFrame *frame, enum AVVideoEncParamsType type,
                                     unsigned int nb_blocks);

#endif /* AVUTIL_VIDEO_ENC_PARAMS_H */
