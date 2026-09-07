/*
 * Copyright (c) 2012 Justin Ruggles
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


#ifndef AVCODEC_VORBIS_PARSER_INTERNAL_H
#define AVCODEC_VORBIS_PARSER_INTERNAL_H

#include "avcodec.h"
#include "vorbis_parser.h"

struct AVVorbisParseContext {
    const AVClass *class;
    int extradata_parsed;       
    int valid_extradata;        
    int blocksize[2];           
    int previous_blocksize;     
    int mode_blocksize[64];     
    int mode_count;             
    int mode_mask;              
    int prev_mask;              
};

#endif /* AVCODEC_VORBIS_PARSER_INTERNAL_H */
