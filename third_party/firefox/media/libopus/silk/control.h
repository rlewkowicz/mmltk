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

#ifndef SILK_CONTROL_H
#define SILK_CONTROL_H

#include "typedef.h"


#define FLAG_DECODE_NORMAL                      0
#define FLAG_PACKET_LOST                        1
#define FLAG_DECODE_LBRR                        2

typedef struct {
    opus_int32 nChannelsAPI;

    opus_int32 nChannelsInternal;

    opus_int32 API_sampleRate;

    opus_int32 maxInternalSampleRate;

    opus_int32 minInternalSampleRate;

    opus_int32 desiredInternalSampleRate;

    opus_int payloadSize_ms;

    opus_int32 bitRate;

    opus_int packetLossPercentage;

    opus_int complexity;

    opus_int useInBandFEC;

    opus_int useDRED;

    opus_int LBRR_coded;

    opus_int useDTX;

    opus_int useCBR;

    opus_int maxBits;

    opus_int toMono;

    opus_int opusCanSwitch;

    opus_int reducedDependency;

    opus_int32 internalSampleRate;

    opus_int allowBandwidthSwitch;

    opus_int inWBmodeWithoutVariableLP;

    opus_int stereoWidth_Q14;

    opus_int switchReady;

    opus_int signalType;

    opus_int offset;
} silk_EncControlStruct;

typedef struct {
    opus_int32 nChannelsAPI;

    opus_int32 nChannelsInternal;

    opus_int32 API_sampleRate;

    opus_int32 internalSampleRate;

    opus_int payloadSize_ms;

    opus_int prevPitchLag;

    opus_int enable_deep_plc;

#ifdef ENABLE_OSCE
    opus_int osce_method;

#ifdef ENABLE_OSCE_BWE
    opus_int enable_osce_bwe;

    opus_int osce_extended_mode;

    opus_int prev_osce_extended_mode;
#endif
#endif
} silk_DecControlStruct;

#endif
