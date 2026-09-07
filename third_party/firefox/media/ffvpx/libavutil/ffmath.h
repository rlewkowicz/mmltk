/*
 * copyright (c) 2016 Ganesh Ajjanagadde <gajjanag@gmail.com>
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


#ifndef AVUTIL_FFMATH_H
#define AVUTIL_FFMATH_H

#include "attributes.h"
#include "libm.h"

static av_always_inline double ff_exp10(double x)
{
    return exp2(M_LOG2_10 * x);
}

static av_always_inline float ff_exp10f(float x)
{
    return exp2f(M_LOG2_10 * x);
}

static av_always_inline float ff_fast_powf(float x, float y)
{
    return expf(logf(x) * y);
}

#endif /* AVUTIL_FFMATH_H */
