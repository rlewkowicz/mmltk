/* Copyright (c) 2011, The WebRTC project authors. All rights reserved.
 * Copyright (c) 2014, Mozilla
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 ** Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *
 ** Redistributions in binary form must reproduce the above copyright
 *  notice, this list of conditions and the following disclaimer in
 *  the documentation and/or other materials provided with the
 *  distribution.
 *
 ** Neither the name of Google nor the names of its contributors may
 *  be used to endorse or promote products derived from this software
 *  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef GMP_VIDEO_CODEC_h_
#define GMP_VIDEO_CODEC_h_

#include <stddef.h>
#include <stdint.h>

enum { kGMPPayloadNameSize = 32 };
enum { kGMPMaxSimulcastStreams = 4 };

enum GMPVideoCodecComplexity {
  kGMPComplexityNormal = 0,
  kGMPComplexityHigh = 1,
  kGMPComplexityHigher = 2,
  kGMPComplexityMax = 3,
  kGMPComplexityInvalid  
};

enum GMPVP8ResilienceMode {
  kResilienceOff,     
  kResilientStream,   
  kResilientFrames,   
  kResilienceInvalid  
};

struct GMPVideoCodecVP8 {
  bool mPictureLossIndicationOn;
  bool mFeedbackModeOn;
  GMPVideoCodecComplexity mComplexity;
  GMPVP8ResilienceMode mResilience;
  uint32_t mNumberOfTemporalLayers;
  bool mDenoisingOn;
  bool mErrorConcealmentOn;
  bool mAutomaticResizeOn;
};


struct GMPVideoCodecH264AVCC {
  uint8_t mVersion;  
  uint8_t mProfile;  
  uint8_t mConstraints;
  uint8_t mLevel;
  uint8_t mLengthSizeMinusOne;  

  uint8_t mNumSPS;  

};

struct GMPVideoCodecH264 {
  uint8_t mPacketizationMode;  
  struct GMPVideoCodecH264AVCC
      mAVCC;  
};

enum GMPVideoCodecType {
  kGMPVideoCodecVP8,

  kGMPVideoCodecH264,
  kGMPVideoCodecVP9,
  kGMPVideoCodecInvalid  
};

struct GMPSimulcastStream {
  uint32_t mWidth;
  uint32_t mHeight;
  uint32_t mNumberOfTemporalLayers;
  uint32_t mMaxBitrate;     
  uint32_t mTargetBitrate;  
  uint32_t mMinBitrate;     
  uint32_t mQPMax;          
};

enum GMPVideoCodecMode {
  kGMPRealtimeVideo,
  kGMPScreensharing,
  kGMPStreamingVideo,
  kGMPNonRealtimeVideo,
  kGMPCodecModeInvalid  
};

enum GMPLogLevel {
  kGMPLogDefault,
  kGMPLogQuiet,
  kGMPLogError,
  kGMPLogWarning,
  kGMPLogInfo,
  kGMPLogDebug,
  kGMPLogDetail,
  kGMPLogInvalid  
};

enum GMPProfile {
  kGMPH264ProfileUnknown,
  kGMPH264ProfileBaseline,
  kGMPH264ProfileMain,
  kGMPH264ProfileExtended,
  kGMPH264ProfileHigh,
  kGMPH264ProfileHigh10,
  kGMPH264ProfileHigh422,
  kGMPH264ProfileHigh444,
  kGMPH264ProfileCavlc444,
  kGMPH264ProfileScalableBaseline,
  kGMPH264ProfileScalableHigh
};

enum GMPLevel {
  kGMPH264LevelUnknown,
  kGMPH264Level1_0,
  kGMPH264Level1_B,
  kGMPH264Level1_1,
  kGMPH264Level1_2,
  kGMPH264Level1_3,
  kGMPH264Level2_0,
  kGMPH264Level2_1,
  kGMPH264Level2_2,
  kGMPH264Level3_0,
  kGMPH264Level3_1,
  kGMPH264Level3_2,
  kGMPH264Level4_0,
  kGMPH264Level4_1,
  kGMPH264Level4_2,
  kGMPH264Level5_0,
  kGMPH264Level5_1,
  kGMPH264Level5_2
};

enum GMPRateControlMode {
  kGMPRateControlUnknown,
  kGMPRateControlBitrate,
  kGMPRateControlQuality,
  kGMPRateControlBufferBased,
  kGMPRateControlTimestamp,
  kGMPRateControlBitratePostskip,
  kGMPRateControlOff
};

enum GMPSliceMode {
  kGMPSliceUnknown,
  kGMPSliceSingle,
  kGMPSliceFixedSlcNum,
  kGMPSliceRaster,
  kGMPSliceSizeLimited
};

enum GMPApiVersion {
  kGMPVersion32 =
      1,  
  kGMPVersion33 = 33,

  kGMPVersion34 = 34,

  kGMPVersion35 = 35,

  kGMPVersion36 = 36,
};

struct GMPVideoCodec {
  uint32_t mGMPApiVersion;

  GMPVideoCodecType mCodecType;
  char mPLName[kGMPPayloadNameSize];  
  uint32_t mPLType;

  uint32_t mWidth;
  uint32_t mHeight;

  uint32_t mStartBitrate;  
  uint32_t mMaxBitrate;    
  uint32_t mMinBitrate;    
  uint32_t mMaxFramerate;

  bool mFrameDroppingOn;
  int32_t mKeyFrameInterval;

  uint32_t mQPMax;
  uint32_t mNumberOfSimulcastStreams;
  GMPSimulcastStream mSimulcastStream[kGMPMaxSimulcastStreams];

  GMPVideoCodecMode mMode;

  bool mUseThreadedDecode;
  GMPLogLevel mLogLevel;

  GMPLevel mLevel;
  GMPProfile mProfile;
  GMPRateControlMode mRateControlMode;
  GMPSliceMode mSliceMode;
  bool mUseThreadedEncode;

  int32_t mTemporalLayerNum;
};

enum GMPBufferType {
  GMP_BufferSingle = 0,
  GMP_BufferLength8,
  GMP_BufferLength16,
  GMP_BufferLength24,
  GMP_BufferLength32,
  GMP_BufferInvalid,
};

struct GMPCodecSpecificInfoGeneric {
  uint8_t mSimulcastIdx;
};

struct GMPCodecSpecificInfoH264 {
  uint8_t mSimulcastIdx;
};

struct GMPCodecSpecificInfoVP8 {
  bool mHasReceivedSLI;
  uint8_t mPictureIdSLI;
  bool mHasReceivedRPSI;
  uint64_t mPictureIdRPSI;
  int16_t mPictureId;  
  bool mNonReference;
  uint8_t mSimulcastIdx;
  uint8_t mTemporalIdx;
  bool mLayerSync;
  int32_t mTL0PicIdx;  
  int8_t mKeyIdx;      
};

union GMPCodecSpecificInfoUnion {
  GMPCodecSpecificInfoGeneric mGeneric;
  GMPCodecSpecificInfoVP8 mVP8;
  GMPCodecSpecificInfoH264 mH264;
};

struct GMPCodecSpecificInfo {
  GMPVideoCodecType mCodecType;
  GMPBufferType mBufferType;
  GMPCodecSpecificInfoUnion mCodecSpecific;
};

#endif  // GMP_VIDEO_CODEC_h_
