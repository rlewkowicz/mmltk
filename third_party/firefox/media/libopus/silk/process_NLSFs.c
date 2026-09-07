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

void silk_process_NLSFs(
    silk_encoder_state          *psEncC,                            
    opus_int16                  PredCoef_Q12[ 2 ][ MAX_LPC_ORDER ], 
    opus_int16                  pNLSF_Q15[         MAX_LPC_ORDER ], 
    const opus_int16            prev_NLSFq_Q15[    MAX_LPC_ORDER ]  
)
{
    opus_int     i, doInterpolate;
    opus_int     NLSF_mu_Q20;
    opus_int16   i_sqr_Q15;
    opus_int16   pNLSF0_temp_Q15[ MAX_LPC_ORDER ];
    opus_int16   pNLSFW_QW[ MAX_LPC_ORDER ];
    opus_int16   pNLSFW0_temp_QW[ MAX_LPC_ORDER ];

    silk_assert( psEncC->speech_activity_Q8 >=   0 );
    silk_assert( psEncC->speech_activity_Q8 <= SILK_FIX_CONST( 1.0, 8 ) );
    celt_assert( psEncC->useInterpolatedNLSFs == 1 || psEncC->indices.NLSFInterpCoef_Q2 == ( 1 << 2 ) );

    NLSF_mu_Q20 = silk_SMLAWB( SILK_FIX_CONST( 0.003, 20 ), SILK_FIX_CONST( -0.001, 28 ), psEncC->speech_activity_Q8 );
    if( psEncC->nb_subfr == 2 ) {
        NLSF_mu_Q20 = silk_ADD_RSHIFT( NLSF_mu_Q20, NLSF_mu_Q20, 1 );
    }

    celt_assert( NLSF_mu_Q20 >  0 );
    silk_assert( NLSF_mu_Q20 <= SILK_FIX_CONST( 0.005, 20 ) );

    silk_NLSF_VQ_weights_laroia( pNLSFW_QW, pNLSF_Q15, psEncC->predictLPCOrder );

    doInterpolate = ( psEncC->useInterpolatedNLSFs == 1 ) && ( psEncC->indices.NLSFInterpCoef_Q2 < 4 );
    if( doInterpolate ) {
        silk_interpolate( pNLSF0_temp_Q15, prev_NLSFq_Q15, pNLSF_Q15,
            psEncC->indices.NLSFInterpCoef_Q2, psEncC->predictLPCOrder );

        silk_NLSF_VQ_weights_laroia( pNLSFW0_temp_QW, pNLSF0_temp_Q15, psEncC->predictLPCOrder );

        i_sqr_Q15 = silk_LSHIFT( silk_SMULBB( psEncC->indices.NLSFInterpCoef_Q2, psEncC->indices.NLSFInterpCoef_Q2 ), 11 );
        for( i = 0; i < psEncC->predictLPCOrder; i++ ) {
            pNLSFW_QW[ i ] = silk_ADD16( silk_RSHIFT( pNLSFW_QW[ i ], 1 ), silk_RSHIFT(
                  silk_SMULBB( pNLSFW0_temp_QW[ i ], i_sqr_Q15 ), 16) );
            silk_assert( pNLSFW_QW[ i ] >= 1 );
        }
    }

    silk_NLSF_encode( psEncC->indices.NLSFIndices, pNLSF_Q15, psEncC->psNLSF_CB, pNLSFW_QW,
        NLSF_mu_Q20, psEncC->NLSF_MSVQ_Survivors, psEncC->indices.signalType );

    silk_NLSF2A( PredCoef_Q12[ 1 ], pNLSF_Q15, psEncC->predictLPCOrder, psEncC->arch );

    if( doInterpolate ) {
        silk_interpolate( pNLSF0_temp_Q15, prev_NLSFq_Q15, pNLSF_Q15,
            psEncC->indices.NLSFInterpCoef_Q2, psEncC->predictLPCOrder );

        silk_NLSF2A( PredCoef_Q12[ 0 ], pNLSF0_temp_Q15, psEncC->predictLPCOrder, psEncC->arch );

    } else {
        celt_assert( psEncC->predictLPCOrder <= MAX_LPC_ORDER );
        silk_memcpy( PredCoef_Q12[ 0 ], PredCoef_Q12[ 1 ], psEncC->predictLPCOrder * sizeof( opus_int16 ) );
    }
}
