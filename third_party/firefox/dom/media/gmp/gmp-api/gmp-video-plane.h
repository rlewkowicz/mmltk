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

#ifndef GMP_VIDEO_PLANE_h_
#define GMP_VIDEO_PLANE_h_

#include <stdint.h>

#include "gmp-errors.h"

class GMPPlane {
 public:
  virtual GMPErr CreateEmptyPlane(int32_t aAllocatedSize, int32_t aStride,
                                  int32_t aPlaneSize) = 0;

  virtual GMPErr Copy(const GMPPlane& aPlane) = 0;

  virtual GMPErr Copy(int32_t aSize, int32_t aStride,
                      const uint8_t* aBuffer) = 0;

  virtual void Swap(GMPPlane& aPlane) = 0;

  virtual int32_t AllocatedSize() const = 0;

  virtual void ResetSize() = 0;

  virtual bool IsZeroSize() const = 0;

  virtual int32_t Stride() const = 0;

  virtual const uint8_t* Buffer() const = 0;

  virtual uint8_t* Buffer() = 0;

  virtual void Destroy() = 0;
};

#endif  // GMP_VIDEO_PLANE_h_
