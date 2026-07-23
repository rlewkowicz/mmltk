/*
 * FLAC (Free Lossless Audio Codec) decoder/parser common functions
 * Copyright (c) 2008 Justin Ruggles
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


#ifndef AVCODEC_FLAC_PARSE_H
#define AVCODEC_FLAC_PARSE_H

#include "avcodec.h"
#include "get_bits.h"

typedef struct FLACStreaminfo {
    int samplerate;         
    int channels;           
    int bps;                
    int max_blocksize;      
    int max_framesize;      
    int64_t samples;        
} FLACStreaminfo;

typedef struct FLACFrameInfo {
    int samplerate;         
    int channels;           
    int bps;                
    int blocksize;          
    int ch_mode;            
    int64_t frame_or_sample_num;    
    int is_var_size;                
} FLACFrameInfo;

int ff_flac_parse_streaminfo(AVCodecContext *avctx, struct FLACStreaminfo *s,
                              const uint8_t *buffer);

int ff_flac_is_extradata_valid(AVCodecContext *avctx,
                               uint8_t **streaminfo_start);

int ff_flac_decode_frame_header(void *logctx, GetBitContext *gb,
                                FLACFrameInfo *fi, int log_level_offset);

void ff_flac_set_channel_layout(AVCodecContext *avctx, int channels);

#endif /* AVCODEC_FLAC_PARSE_H */
