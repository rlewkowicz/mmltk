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

#ifndef AVCODEC_BSF_INTERNAL_H
#define AVCODEC_BSF_INTERNAL_H

#include "libavutil/log.h"

#include "bsf.h"
#include "packet.h"

typedef struct FFBitStreamFilter {
    AVBitStreamFilter p;

    int priv_data_size;
    int (*init)(AVBSFContext *ctx);
    int (*filter)(AVBSFContext *ctx, AVPacket *pkt);
    void (*close)(AVBSFContext *ctx);
    void (*flush)(AVBSFContext *ctx);
} FFBitStreamFilter;

int ff_bsf_get_packet(AVBSFContext *ctx, AVPacket **pkt);

int ff_bsf_get_packet_ref(AVBSFContext *ctx, AVPacket *pkt);

const AVClass *ff_bsf_child_class_iterate(void **opaque);

#endif /* AVCODEC_BSF_INTERNAL_H */
