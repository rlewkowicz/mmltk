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

#ifndef GMP_VIDEO_FRAME_I420_h_
#define GMP_VIDEO_FRAME_I420_h_

#include <stdint.h>

#include "gmp-errors.h"
#include "gmp-video-frame.h"
#include "gmp-video-plane.h"

enum GMPPlaneType {
  kGMPYPlane = 0,
  kGMPUPlane = 1,
  kGMPVPlane = 2,
  kGMPNumOfPlanes = 3
};

class GMPVideoi420Frame : public GMPVideoFrame {
 public:
  virtual GMPErr CreateEmptyFrame(int32_t aWidth, int32_t aHeight,
                                  int32_t aStride_y, int32_t aStride_u,
                                  int32_t aStride_v) = 0;

  virtual GMPErr CreateFrame(int32_t aSize_y, const uint8_t* aBuffer_y,
                             int32_t aSize_u, const uint8_t* aBuffer_u,
                             int32_t aSize_v, const uint8_t* aBuffer_v,
                             int32_t aWidth, int32_t aHeight, int32_t aStride_y,
                             int32_t aStride_u, int32_t aStride_v) = 0;

  virtual GMPErr CopyFrame(const GMPVideoi420Frame& aVideoFrame) = 0;

  virtual void SwapFrame(GMPVideoi420Frame* aVideoFrame) = 0;

  virtual uint8_t* Buffer(GMPPlaneType aType) = 0;

  virtual const uint8_t* Buffer(GMPPlaneType aType) const = 0;

  virtual int32_t AllocatedSize(GMPPlaneType aType) const = 0;

  virtual int32_t Stride(GMPPlaneType aType) const = 0;

  virtual GMPErr SetWidth(int32_t aWidth) = 0;

  virtual GMPErr SetHeight(int32_t aHeight) = 0;

  virtual int32_t Width() const = 0;

  virtual int32_t Height() const = 0;

  virtual void SetTimestamp(uint64_t aTimestamp) = 0;

  virtual uint64_t Timestamp() const = 0;

  virtual void SetDuration(uint64_t aDuration) = 0;

  virtual uint64_t Duration() const = 0;

  virtual bool IsZeroSize() const = 0;

  virtual void ResetSize() = 0;


  virtual void SetUpdatedTimestamp(uint64_t aTimestamp) = 0;

  virtual uint64_t UpdatedTimestamp() const = 0;
};

#endif  // GMP_VIDEO_FRAME_I420_h_
