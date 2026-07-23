/*
 * AVCodecParser prototypes and definitions
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2003 Michael Niedermayer
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

#ifndef AVCODEC_PARSER_H
#define AVCODEC_PARSER_H

#include "avcodec.h"

typedef struct ParseContext{
    uint8_t *buffer;
    int index;
    int last_index;
    unsigned int buffer_size;
    uint32_t state;             
    int frame_start_found;
    int overread;               
    int overread_index;         
    uint64_t state64;           
} ParseContext;

#define END_NOT_FOUND (-100)

int ff_combine_frame(ParseContext *pc, int next, const uint8_t **buf, int *buf_size);
void ff_parse_close(AVCodecParserContext *s);

void ff_fetch_timestamp(AVCodecParserContext *s, int off, int remove, int fuzzy);

#endif /* AVCODEC_PARSER_H */
