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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "main_FLP.h"

void silk_LTP_analysis_filter_FLP(
    silk_float                      *LTP_res,                           
    const silk_float                *x,                                 
    const silk_float                B[ LTP_ORDER * MAX_NB_SUBFR ],      
    const opus_int                  pitchL[   MAX_NB_SUBFR ],           
    const silk_float                invGains[ MAX_NB_SUBFR ],           
    const opus_int                  subfr_length,                       
    const opus_int                  nb_subfr,                           
    const opus_int                  pre_length                          
)
{
    const silk_float *x_ptr, *x_lag_ptr;
    silk_float   Btmp[ LTP_ORDER ];
    silk_float   *LTP_res_ptr;
    silk_float   inv_gain;
    opus_int     k, i, j;

    x_ptr = x;
    LTP_res_ptr = LTP_res;
    for( k = 0; k < nb_subfr; k++ ) {
        x_lag_ptr = x_ptr - pitchL[ k ];
        inv_gain = invGains[ k ];
        for( i = 0; i < LTP_ORDER; i++ ) {
            Btmp[ i ] = B[ k * LTP_ORDER + i ];
        }

        for( i = 0; i < subfr_length + pre_length; i++ ) {
            LTP_res_ptr[ i ] = x_ptr[ i ];
            for( j = 0; j < LTP_ORDER; j++ ) {
                LTP_res_ptr[ i ] -= Btmp[ j ] * x_lag_ptr[ LTP_ORDER / 2 - j ];
            }
            LTP_res_ptr[ i ] *= inv_gain;
            x_lag_ptr++;
        }

        LTP_res_ptr += subfr_length + pre_length;
        x_ptr       += subfr_length;
    }
}
