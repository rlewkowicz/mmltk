/***********************************************************************
Copyright (c) 2006-2011, Skype Limited. All rights reserved.
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
- Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
- Neither the name of Internet Society, IETF or IETF Trust, nor the
names of specific contributors, may be used to endorse or promote
products derived from this software without specific prior written
permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
***********************************************************************/

/*! \file silk_Inlines.h
 *  \brief silk_Inlines.h defines OPUS_INLINE signal processing functions.
 */

#ifndef SILK_FIX_INLINES_H
#define SILK_FIX_INLINES_H


static OPUS_INLINE opus_int32 silk_CLZ64( opus_int64 in )
{
    opus_int32 in_upper;

    in_upper = (opus_int32)silk_RSHIFT64(in, 32);
    if (in_upper == 0) {
        return 32 + silk_CLZ32( (opus_int32) in );
    } else {
        return silk_CLZ32( in_upper );
    }
}

static OPUS_INLINE void silk_CLZ_FRAC(
    opus_int32 in,            
    opus_int32 *lz,           
    opus_int32 *frac_Q7       
)
{
    opus_int32 lzeros = silk_CLZ32(in);

    * lz = lzeros;
    * frac_Q7 = silk_ROR32(in, 24 - lzeros) & 0x7f;
}

static OPUS_INLINE opus_int32 silk_SQRT_APPROX( opus_int32 x )
{
    opus_int32 y, lz, frac_Q7;

    if( x <= 0 ) {
        return 0;
    }

    silk_CLZ_FRAC(x, &lz, &frac_Q7);

    if( lz & 1 ) {
        y = 32768;
    } else {
        y = 46214;        
    }

    y >>= silk_RSHIFT(lz, 1);

    y = silk_SMLAWB(y, y, silk_SMULBB(213, frac_Q7));

    return y;
}

static OPUS_INLINE opus_int32 silk_DIV32_varQ(   
    const opus_int32     a32,               
    const opus_int32     b32,               
    const opus_int       Qres               
)
{
    opus_int   a_headrm, b_headrm, lshift;
    opus_int32 b32_inv, a32_nrm, b32_nrm, result;

    silk_assert( b32 != 0 );
    silk_assert( Qres >= 0 );

    a_headrm = silk_CLZ32( silk_abs(a32) ) - 1;
    a32_nrm = silk_LSHIFT(a32, a_headrm);                                       
    b_headrm = silk_CLZ32( silk_abs(b32) ) - 1;
    b32_nrm = silk_LSHIFT(b32, b_headrm);                                       

    b32_inv = silk_DIV32_16( silk_int32_MAX >> 2, silk_RSHIFT(b32_nrm, 16) );   

    result = silk_SMULWB(a32_nrm, b32_inv);                                     

    a32_nrm = silk_SUB32_ovflw(a32_nrm, silk_LSHIFT_ovflw( silk_SMMUL(b32_nrm, result), 3 ));  

    result = silk_SMLAWB(result, a32_nrm, b32_inv);                             

    lshift = 29 + a_headrm - b_headrm - Qres;
    if( lshift < 0 ) {
        return silk_LSHIFT_SAT32(result, -lshift);
    } else {
        if( lshift < 32){
            return silk_RSHIFT(result, lshift);
        } else {
            return 0;
        }
    }
}

static OPUS_INLINE opus_int32 silk_INVERSE32_varQ(   
    const opus_int32     b32,                   
    const opus_int       Qres                   
)
{
    opus_int   b_headrm, lshift;
    opus_int32 b32_inv, b32_nrm, err_Q32, result;

    silk_assert( b32 != 0 );
    silk_assert( Qres > 0 );

    b_headrm = silk_CLZ32( silk_abs(b32) ) - 1;
    b32_nrm = silk_LSHIFT(b32, b_headrm);                                       

    b32_inv = silk_DIV32_16( silk_int32_MAX >> 2, silk_RSHIFT(b32_nrm, 16) );   

    result = silk_LSHIFT(b32_inv, 16);                                          

    err_Q32 = silk_LSHIFT( ((opus_int32)1<<29) - silk_SMULWB(b32_nrm, b32_inv), 3 );        

    result = silk_SMLAWW(result, err_Q32, b32_inv);                             

    lshift = 61 - b_headrm - Qres;
    if( lshift <= 0 ) {
        return silk_LSHIFT_SAT32(result, -lshift);
    } else {
        if( lshift < 32){
            return silk_RSHIFT(result, lshift);
        }else{
            return 0;
        }
    }
}

#endif /* SILK_FIX_INLINES_H */
