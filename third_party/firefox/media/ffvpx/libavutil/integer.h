/*
 * arbitrary precision integers
 * Copyright (c) 2004 Michael Niedermayer <michaelni@gmx.at>
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


#ifndef AVUTIL_INTEGER_H
#define AVUTIL_INTEGER_H

#include <stdint.h>
#include "attributes.h"

#define AV_INTEGER_SIZE 8

typedef struct AVInteger{
    uint16_t v[AV_INTEGER_SIZE];
} AVInteger;

AVInteger av_add_i(AVInteger a, AVInteger b) av_const;
AVInteger av_sub_i(AVInteger a, AVInteger b) av_const;

int av_log2_i(AVInteger a) av_const;
AVInteger av_mul_i(AVInteger a, AVInteger b) av_const;

int av_cmp_i(AVInteger a, AVInteger b) av_const;

AVInteger av_shr_i(AVInteger a, int s) av_const;

AVInteger av_mod_i(AVInteger *quot, AVInteger a, AVInteger b);

AVInteger av_div_i(AVInteger a, AVInteger b) av_const;

AVInteger av_int2i(int64_t a) av_const;

int64_t av_i2int(AVInteger a) av_const;

#endif /* AVUTIL_INTEGER_H */
