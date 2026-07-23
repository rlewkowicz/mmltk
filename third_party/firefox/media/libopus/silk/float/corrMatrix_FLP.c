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

void silk_corrVector_FLP(
    const silk_float                *x,                                 
    const silk_float                *t,                                 
    const opus_int                  L,                                  
    const opus_int                  Order,                              
    silk_float                      *Xt,                                
    int                             arch
)
{
    opus_int lag;
    const silk_float *ptr1;

    ptr1 = &x[ Order - 1 ];                     
    for( lag = 0; lag < Order; lag++ ) {
        Xt[ lag ] = (silk_float)silk_inner_product_FLP( ptr1, t, L, arch );
        ptr1--;                                 
    }
}

void silk_corrMatrix_FLP(
    const silk_float                *x,                                 
    const opus_int                  L,                                  
    const opus_int                  Order,                              
    silk_float                      *XX,                                
    int                             arch
)
{
    opus_int j, lag;
    double  energy;
    const silk_float *ptr1, *ptr2;

    ptr1 = &x[ Order - 1 ];                     
    energy = silk_energy_FLP( ptr1, L );  
    matrix_ptr( XX, 0, 0, Order ) = ( silk_float )energy;
    for( j = 1; j < Order; j++ ) {
        energy += ptr1[ -j ] * ptr1[ -j ] - ptr1[ L - j ] * ptr1[ L - j ];
        matrix_ptr( XX, j, j, Order ) = ( silk_float )energy;
    }

    ptr2 = &x[ Order - 2 ];                     
    for( lag = 1; lag < Order; lag++ ) {
        energy = silk_inner_product_FLP( ptr1, ptr2, L, arch );
        matrix_ptr( XX, lag, 0, Order ) = ( silk_float )energy;
        matrix_ptr( XX, 0, lag, Order ) = ( silk_float )energy;
        for( j = 1; j < ( Order - lag ); j++ ) {
            energy += ptr1[ -j ] * ptr2[ -j ] - ptr1[ L - j ] * ptr2[ L - j ];
            matrix_ptr( XX, lag + j, j, Order ) = ( silk_float )energy;
            matrix_ptr( XX, j, lag + j, Order ) = ( silk_float )energy;
        }
        ptr2--;                                 
    }
}
