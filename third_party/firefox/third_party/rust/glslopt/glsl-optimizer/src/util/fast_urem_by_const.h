/*
 * Copyright © 2010 Valve Software
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

#include <stdint.h>


#define REMAINDER_MAGIC(divisor) \
   ((uint64_t) ~0ull / (divisor) + 1)


static inline uint32_t
_mul32by64_hi(uint32_t a, uint64_t b)
{
#ifdef HAVE_UINT128
   return ((__uint128_t) b * a) >> 64;
#else
   uint32_t b0 = (uint32_t) b;
   uint32_t b1 = b >> 32;
   return ((((uint64_t) a * b0) >> 32) + (uint64_t) a * b1) >> 32;
#endif
}

static inline uint32_t
util_fast_urem32(uint32_t n, uint32_t d, uint64_t magic)
{
   uint64_t lowbits = magic * n;
   uint32_t result = _mul32by64_hi(d, lowbits);
   assert(result == n % d);
   return result;
}

