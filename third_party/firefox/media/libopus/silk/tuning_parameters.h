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

#ifndef SILK_TUNING_PARAMETERS_H
#define SILK_TUNING_PARAMETERS_H

#define BITRESERVOIR_DECAY_TIME_MS                      500


#define FIND_PITCH_WHITE_NOISE_FRACTION                 1e-3f

#define FIND_PITCH_BANDWIDTH_EXPANSION                  0.99f


#define FIND_LPC_COND_FAC                               1e-5f

#define MAX_SUM_LOG_GAIN_DB                             250.0f

#define LTP_CORR_INV_MAX                                0.03f


#define VARIABLE_HP_SMTH_COEF1                          0.1f
#define VARIABLE_HP_SMTH_COEF2                          0.015f
#define VARIABLE_HP_MAX_DELTA_FREQ                      0.4f

#define VARIABLE_HP_MIN_CUTOFF_HZ                       60
#define VARIABLE_HP_MAX_CUTOFF_HZ                       100


#define SPEECH_ACTIVITY_DTX_THRES                       0.05f

#define LBRR_SPEECH_ACTIVITY_THRES                      0.3f


#define BG_SNR_DECR_dB                                  2.0f

#define HARM_SNR_INCR_dB                                2.0f

#define SPARSE_SNR_INCR_dB                              2.0f

#define ENERGY_VARIATION_THRESHOLD_QNT_OFFSET           0.6f

#define WARPING_MULTIPLIER                              0.015f

#define SHAPE_WHITE_NOISE_FRACTION                      3e-5f

#define BANDWIDTH_EXPANSION                             0.94f

#define HARMONIC_SHAPING                                0.3f

#define HIGH_RATE_OR_LOW_QUALITY_HARMONIC_SHAPING       0.2f

#define HP_NOISE_COEF                                   0.25f

#define HARM_HP_NOISE_COEF                              0.35f

#define INPUT_TILT                                      0.05f

#define HIGH_RATE_INPUT_TILT                            0.1f

#define LOW_FREQ_SHAPING                                4.0f

#define LOW_QUALITY_LOW_FREQ_SHAPING_DECR               0.5f

#define SUBFR_SMTH_COEF                                 0.4f

#define LAMBDA_OFFSET                                   1.2f
#define LAMBDA_SPEECH_ACT                               -0.2f
#define LAMBDA_DELAYED_DECISIONS                        -0.05f
#define LAMBDA_INPUT_QUALITY                            -0.1f
#define LAMBDA_CODING_QUALITY                           -0.2f
#define LAMBDA_QUANT_OFFSET                             0.8f

#define REDUCE_BITRATE_10_MS_BPS                        2200

#define MAX_BANDWIDTH_SWITCH_DELAY_MS                   5000

#endif /* SILK_TUNING_PARAMETERS_H */
