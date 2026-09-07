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


#include "main.h"

static OPUS_INLINE void silk_LP_interpolate_filter_taps(
    opus_int32           B_Q28[ TRANSITION_NB ],
    opus_int32           A_Q28[ TRANSITION_NA ],
    const opus_int       ind,
    const opus_int32     fac_Q16
)
{
    opus_int nb, na;

    if( ind < TRANSITION_INT_NUM - 1 ) {
        if( fac_Q16 > 0 ) {
            if( fac_Q16 < 32768 ) { 
                for( nb = 0; nb < TRANSITION_NB; nb++ ) {
                    B_Q28[ nb ] = silk_SMLAWB(
                        silk_Transition_LP_B_Q28[ ind     ][ nb ],
                        silk_Transition_LP_B_Q28[ ind + 1 ][ nb ] -
                        silk_Transition_LP_B_Q28[ ind     ][ nb ],
                        fac_Q16 );
                }
                for( na = 0; na < TRANSITION_NA; na++ ) {
                    A_Q28[ na ] = silk_SMLAWB(
                        silk_Transition_LP_A_Q28[ ind     ][ na ],
                        silk_Transition_LP_A_Q28[ ind + 1 ][ na ] -
                        silk_Transition_LP_A_Q28[ ind     ][ na ],
                        fac_Q16 );
                }
            } else { 
                silk_assert( fac_Q16 - ( 1 << 16 ) == silk_SAT16( fac_Q16 - ( 1 << 16 ) ) );
                for( nb = 0; nb < TRANSITION_NB; nb++ ) {
                    B_Q28[ nb ] = silk_SMLAWB(
                        silk_Transition_LP_B_Q28[ ind + 1 ][ nb ],
                        silk_Transition_LP_B_Q28[ ind + 1 ][ nb ] -
                        silk_Transition_LP_B_Q28[ ind     ][ nb ],
                        fac_Q16 - ( (opus_int32)1 << 16 ) );
                }
                for( na = 0; na < TRANSITION_NA; na++ ) {
                    A_Q28[ na ] = silk_SMLAWB(
                        silk_Transition_LP_A_Q28[ ind + 1 ][ na ],
                        silk_Transition_LP_A_Q28[ ind + 1 ][ na ] -
                        silk_Transition_LP_A_Q28[ ind     ][ na ],
                        fac_Q16 - ( (opus_int32)1 << 16 ) );
                }
            }
        } else {
            silk_memcpy( B_Q28, silk_Transition_LP_B_Q28[ ind ], TRANSITION_NB * sizeof( opus_int32 ) );
            silk_memcpy( A_Q28, silk_Transition_LP_A_Q28[ ind ], TRANSITION_NA * sizeof( opus_int32 ) );
        }
    } else {
        silk_memcpy( B_Q28, silk_Transition_LP_B_Q28[ TRANSITION_INT_NUM - 1 ], TRANSITION_NB * sizeof( opus_int32 ) );
        silk_memcpy( A_Q28, silk_Transition_LP_A_Q28[ TRANSITION_INT_NUM - 1 ], TRANSITION_NA * sizeof( opus_int32 ) );
    }
}

void silk_LP_variable_cutoff(
    silk_LP_state               *psLP,                          
    opus_int16                  *frame,                         
    const opus_int              frame_length                    
)
{
    opus_int32   B_Q28[ TRANSITION_NB ], A_Q28[ TRANSITION_NA ], fac_Q16 = 0;
    opus_int     ind = 0;

    silk_assert( psLP->transition_frame_no >= 0 && psLP->transition_frame_no <= TRANSITION_FRAMES );

    if( psLP->mode != 0 ) {
#if( TRANSITION_INT_STEPS == 64 )
        fac_Q16 = silk_LSHIFT( TRANSITION_FRAMES - psLP->transition_frame_no, 16 - 6 );
#else
        fac_Q16 = silk_DIV32_16( silk_LSHIFT( TRANSITION_FRAMES - psLP->transition_frame_no, 16 ), TRANSITION_FRAMES );
#endif
        ind      = silk_RSHIFT( fac_Q16, 16 );
        fac_Q16 -= silk_LSHIFT( ind, 16 );

        silk_assert( ind >= 0 );
        silk_assert( ind < TRANSITION_INT_NUM );

        silk_LP_interpolate_filter_taps( B_Q28, A_Q28, ind, fac_Q16 );

        psLP->transition_frame_no = silk_LIMIT( psLP->transition_frame_no + psLP->mode, 0, TRANSITION_FRAMES );

        silk_assert( TRANSITION_NB == 3 && TRANSITION_NA == 2 );
        silk_biquad_alt_stride1( frame, B_Q28, A_Q28, psLP->In_LP_State, frame, frame_length);
    }
}
