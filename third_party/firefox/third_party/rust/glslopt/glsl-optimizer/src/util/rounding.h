/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef _ROUNDING_H
#define _ROUNDING_H

#include <limits.h>
#include <math.h>
#include <stdint.h>

#if defined(__SSE__) || (defined(_M_IX86_FP) && (_M_IX86_FP >= 1)) || defined(_M_X64)
#include <xmmintrin.h>
#include <emmintrin.h>
#endif

#ifdef __SSE4_1__
#include <smmintrin.h>
#endif


static inline float
_mesa_roundevenf(float x)
{
#ifdef __SSE4_1__
   float ret;
   __m128 m = _mm_load_ss(&x);
   m = _mm_round_ss(m, m, _MM_FROUND_CUR_DIRECTION | _MM_FROUND_NO_EXC);
   _mm_store_ss(&ret, m);
   return ret;
#else
   return rintf(x);
#endif
}

static inline double
_mesa_roundeven(double x)
{
#ifdef __SSE4_1__
   double ret;
   __m128d m = _mm_load_sd(&x);
   m = _mm_round_sd(m, m, _MM_FROUND_CUR_DIRECTION | _MM_FROUND_NO_EXC);
   _mm_store_sd(&ret, m);
   return ret;
#else
   return rint(x);
#endif
}

static inline long
_mesa_lroundevenf(float x)
{
#if defined(__SSE__) || (defined(_M_IX86_FP) && (_M_IX86_FP >= 1)) || defined(_M_X64)
#if LONG_MAX == INT64_MAX
   return _mm_cvtss_si64(_mm_load_ss(&x));
#elif LONG_MAX == INT32_MAX
   return _mm_cvtss_si32(_mm_load_ss(&x));
#else
#error "Unsupported long size"
#endif
#else
   return lrintf(x);
#endif
}


static inline long
_mesa_lroundeven(double x)
{
#if defined(__SSE2__) || (defined(_M_IX86_FP) && (_M_IX86_FP >= 2)) || defined(_M_X64)
#if LONG_MAX == INT64_MAX
   return _mm_cvtsd_si64(_mm_load_sd(&x));
#elif LONG_MAX == INT32_MAX
   return _mm_cvtsd_si32(_mm_load_sd(&x));
#else
#error "Unsupported long size"
#endif
#else
   return lrint(x);
#endif
}

static inline int64_t
_mesa_i64roundevenf(float x)
{
#if LONG_MAX == INT64_MAX
   return _mesa_lroundevenf(x);
#elif LONG_MAX == INT32_MAX
   return llrintf(x);
#else
#error "Unsupported long size"
#endif
}

#endif
