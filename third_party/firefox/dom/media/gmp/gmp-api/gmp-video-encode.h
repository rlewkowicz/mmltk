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

#ifndef GMP_VIDEO_ENCODE_h_
#define GMP_VIDEO_ENCODE_h_

#include <stdint.h>

#include "gmp-errors.h"
#include "gmp-video-codec.h"
#include "gmp-video-frame-encoded.h"
#include "gmp-video-frame-i420.h"

class GMPVideoEncoderCallback {
 public:
  virtual ~GMPVideoEncoderCallback() {}

  virtual void Encoded(GMPVideoEncodedFrame* aEncodedFrame,
                       const uint8_t* aCodecSpecificInfo,
                       uint32_t aCodecSpecificInfoLength) = 0;

  virtual void Error(GMPErr aError) = 0;
};

#define GMP_API_VIDEO_ENCODER "encode-video"

class GMPVideoEncoder {
 public:
  virtual ~GMPVideoEncoder() {}

  virtual void InitEncode(const GMPVideoCodec& aCodecSettings,
                          const uint8_t* aCodecSpecific,
                          uint32_t aCodecSpecificLength,
                          GMPVideoEncoderCallback* aCallback,
                          int32_t aNumberOfCores, uint32_t aMaxPayloadSize) = 0;

  virtual void Encode(GMPVideoi420Frame* aInputFrame,
                      const uint8_t* aCodecSpecificInfo,
                      uint32_t aCodecSpecificInfoLength,
                      const GMPVideoFrameType* aFrameTypes,
                      uint32_t aFrameTypesLength) = 0;

  virtual void SetChannelParameters(uint32_t aPacketLoss, uint32_t aRTT) = 0;

  virtual void SetRates(uint32_t aNewBitRate, uint32_t aFrameRate) = 0;

  virtual void SetPeriodicKeyFrames(bool aEnable) = 0;

  virtual void EncodingComplete() = 0;
};

#endif  // GMP_VIDEO_ENCODE_h_
