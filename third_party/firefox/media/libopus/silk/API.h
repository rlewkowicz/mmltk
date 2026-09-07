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

#ifndef SILK_API_H
#define SILK_API_H

#include "control.h"
#include "typedef.h"
#include "errors.h"
#include "entenc.h"
#include "entdec.h"

#ifdef ENABLE_DEEP_PLC
#include "lpcnet_private.h"
#endif

#define SILK_MAX_FRAMES_PER_PACKET  3

typedef struct {
    opus_int    VADFlag;                                
    opus_int    VADFlags[ SILK_MAX_FRAMES_PER_PACKET ]; 
    opus_int    inbandFECFlag;                          
} silk_TOC_struct;


opus_int silk_Get_Encoder_Size(                         
    opus_int                        *encSizeBytes,      
    opus_int                         channels           
);

opus_int silk_InitEncoder(                              
    void                            *encState,          
    int                              channels,          
    int                              arch,              
    silk_EncControlStruct           *encStatus          
);

opus_int silk_Encode(                                   
    void                            *encState,          
    silk_EncControlStruct           *encControl,        
    const opus_res                  *samplesIn,         
    opus_int                        nSamplesIn,         
    ec_enc                          *psRangeEnc,        
    opus_int32                      *nBytesOut,         
    const opus_int                  prefillFlag,        
    int                             activity            
);



opus_int silk_LoadOSCEModels(
    void *decState,                                     
    const unsigned char *data,                          
    int len                                             
);

opus_int silk_Get_Decoder_Size(                         
    opus_int                        *decSizeBytes       
);

opus_int silk_ResetDecoder(                              
    void                            *decState            
);

opus_int silk_InitDecoder(                              
    void                            *decState           
);

opus_int silk_Decode(                                   
    void*                           decState,           
    silk_DecControlStruct*          decControl,         
    opus_int                        lostFlag,           
    opus_int                        newPacketFlag,      
    ec_dec                          *psRangeDec,        
    opus_res                        *samplesOut,        
    opus_int32                      *nSamplesOut,       
#ifdef ENABLE_DEEP_PLC
    LPCNetPLCState                  *lpcnet,
#endif
    int                             arch                
);

#if 0
opus_int silk_get_TOC(
    const opus_uint8                *payload,           
    const opus_int                  nBytesIn,           
    const opus_int                  nFramesPerPayload,  
    silk_TOC_struct                 *Silk_TOC           
);
#endif


#endif
