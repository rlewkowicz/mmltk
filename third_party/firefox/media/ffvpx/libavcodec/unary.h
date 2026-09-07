/*
 * copyright (c) 2004 Michael Niedermayer <michaelni@gmx.at>
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

#ifndef AVCODEC_UNARY_H
#define AVCODEC_UNARY_H

#include "get_bits.h"

static inline int get_unary(GetBitContext *gb, int stop, int len)
{
    int i;

    for(i = 0; i < len && get_bits1(gb) != stop; i++);
    return i;
}

static inline int get_unary_0_33(GetBitContext *gb)
{
    return get_unary(gb, 0, 33);
}

static inline int get_unary_0_9(GetBitContext *gb)
{
    return get_unary(gb, 0, 9);
}

#endif /* AVCODEC_UNARY_H */
