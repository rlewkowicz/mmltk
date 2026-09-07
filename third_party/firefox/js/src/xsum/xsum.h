
/* Copyright 2015, 2018, 2021 Radford M. Neal

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
   OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#ifndef XSUM_H
#define XSUM_H

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>


typedef double xsum_flt; 

typedef int64_t xsum_int;         
typedef uint64_t xsum_uint;       
typedef int_fast16_t xsum_expint; 

#define XSUM_MANTISSA_BITS 52 /* Bits in fp mantissa, excludes implict 1 */
#define XSUM_EXP_BITS 11      /* Bits in fp exponent */

#define XSUM_MANTISSA_MASK \
  (((xsum_int)1 << XSUM_MANTISSA_BITS) - 1) 

#define XSUM_EXP_MASK ((1 << XSUM_EXP_BITS) - 1) /* Mask for exponent */

#define XSUM_EXP_BIAS \
  ((1 << (XSUM_EXP_BITS - 1)) - 1) 

#define XSUM_SIGN_BIT \
  (XSUM_MANTISSA_BITS + XSUM_EXP_BITS) 

#define XSUM_SIGN_MASK ((xsum_uint)1 << XSUM_SIGN_BIT) /* Mask for sign bit */


#define XSUM_SCHUNK_BITS 64  /* Bits in chunk of the small accumulator */
typedef int64_t xsum_schunk; 

#define XSUM_LOW_EXP_BITS 5 /* # of low bits of exponent, in one chunk */

#define XSUM_LOW_EXP_MASK \
  ((1 << XSUM_LOW_EXP_BITS) - 1) 

#define XSUM_HIGH_EXP_BITS \
  (XSUM_EXP_BITS - XSUM_LOW_EXP_BITS) 

#define XSUM_HIGH_EXP_MASK \
  ((1 << HIGH_EXP_BITS) - 1) 

#define XSUM_SCHUNKS \
  ((1 << XSUM_HIGH_EXP_BITS) + 3) 

#define XSUM_LOW_MANTISSA_BITS \
  (1 << XSUM_LOW_EXP_BITS) 

#define XSUM_HIGH_MANTISSA_BITS \
  (XSUM_MANTISSA_BITS - XSUM_LOW_MANTISSA_BITS) 

#define XSUM_LOW_MANTISSA_MASK \
  (((xsum_int)1 << XSUM_LOW_MANTISSA_BITS) - 1) 

#define XSUM_SMALL_CARRY_BITS \
  ((XSUM_SCHUNK_BITS - 1) - XSUM_MANTISSA_BITS) 

#define XSUM_SMALL_CARRY_TERMS \
  ((1 << XSUM_SMALL_CARRY_BITS) - 1) 

typedef struct {
  xsum_schunk chunk[XSUM_SCHUNKS]; 
  xsum_int Inf;                    
  xsum_int NaN;                    
  int adds_until_propagate;        
} xsum_small_accumulator;          


#define XSUM_LCHUNK_BITS 64   /* Bits in chunk of the large accumulator */
typedef uint64_t xsum_lchunk; 

#define XSUM_LCOUNT_BITS (64 - XSUM_MANTISSA_BITS) /* # of bits in count */
typedef int_least16_t xsum_lcount; 

#define XSUM_LCHUNKS \
  (1 << (XSUM_EXP_BITS + 1)) 

typedef uint64_t xsum_used; 

typedef struct {
  xsum_lchunk chunk[XSUM_LCHUNKS]; 
  xsum_lcount count[XSUM_LCHUNKS]; 
  xsum_used chunks_used[XSUM_LCHUNKS / 64]; 
  xsum_used used_used;         
  xsum_small_accumulator sacc; 
} xsum_large_accumulator;


typedef ptrdiff_t xsum_length;


void xsum_small_init(xsum_small_accumulator*);
void xsum_small_add1(xsum_small_accumulator*, xsum_flt);
xsum_flt xsum_small_round(xsum_small_accumulator*);


void xsum_small_display(xsum_small_accumulator*);


extern int xsum_debug;

#endif
