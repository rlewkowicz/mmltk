/*
 * Bitstream filters public API
 *
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

#ifndef AVCODEC_BSF_H
#define AVCODEC_BSF_H

#include "libavutil/dict.h"
#include "libavutil/log.h"
#include "libavutil/rational.h"

#include "codec_id.h"
#include "codec_par.h"
#include "packet.h"


typedef struct AVBSFContext {
    const AVClass *av_class;

    const struct AVBitStreamFilter *filter;

    void *priv_data;

    AVCodecParameters *par_in;

    AVCodecParameters *par_out;

    AVRational time_base_in;

    AVRational time_base_out;
} AVBSFContext;

typedef struct AVBitStreamFilter {
    const char *name;

    const enum AVCodecID *codec_ids;

    const AVClass *priv_class;
} AVBitStreamFilter;

const AVBitStreamFilter *av_bsf_get_by_name(const char *name);

const AVBitStreamFilter *av_bsf_iterate(void **opaque);

int av_bsf_alloc(const AVBitStreamFilter *filter, AVBSFContext **ctx);

int av_bsf_init(AVBSFContext *ctx);

int av_bsf_send_packet(AVBSFContext *ctx, AVPacket *pkt);

int av_bsf_receive_packet(AVBSFContext *ctx, AVPacket *pkt);

void av_bsf_flush(AVBSFContext *ctx);

void av_bsf_free(AVBSFContext **ctx);

const AVClass *av_bsf_get_class(void);

typedef struct AVBSFList AVBSFList;

AVBSFList *av_bsf_list_alloc(void);

void av_bsf_list_free(AVBSFList **lst);

int av_bsf_list_append(AVBSFList *lst, AVBSFContext *bsf);

int av_bsf_list_append2(AVBSFList *lst, const char * bsf_name, AVDictionary **options);
int av_bsf_list_finalize(AVBSFList **lst, AVBSFContext **bsf);

int av_bsf_list_parse_str(const char *str, AVBSFContext **bsf);

int av_bsf_get_null_filter(AVBSFContext **bsf);


#endif // AVCODEC_BSF_H
