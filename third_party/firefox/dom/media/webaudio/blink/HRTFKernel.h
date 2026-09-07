/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef HRTFKernel_h
#define HRTFKernel_h

#include "mozilla/FFTBlock.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/UniquePtr.h"
#include "nsAutoRef.h"
#include "nsTArray.h"

namespace WebCore {

using mozilla::FFTBlock;
using mozilla::UniquePtr;

class HRTFKernel {
 public:
  HRTFKernel(const HRTFKernel& other) = delete;
  void operator=(const HRTFKernel& other) = delete;

  static nsReturnRef<HRTFKernel> create(float* impulseResponse, size_t length,
                                        float sampleRate);

  static nsReturnRef<HRTFKernel> create(UniquePtr<FFTBlock> fftFrame,
                                        float frameDelay, float sampleRate);

  static nsReturnRef<HRTFKernel> createInterpolatedKernel(HRTFKernel* kernel1,
                                                          HRTFKernel* kernel2,
                                                          float x);

  FFTBlock* fftFrame() { return m_fftFrame.get(); }

  size_t fftSize() const { return m_fftFrame->FFTSize(); }
  float frameDelay() const { return m_frameDelay; }

  float sampleRate() const { return m_sampleRate; }
  double nyquist() const { return 0.5 * sampleRate(); }

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const {
    size_t amount = aMallocSizeOf(this);
    amount += m_fftFrame->SizeOfIncludingThis(aMallocSizeOf);
    return amount;
  }

 private:
  HRTFKernel(float* impulseResponse, size_t fftSize, float sampleRate);

  HRTFKernel(UniquePtr<FFTBlock> fftFrame, float frameDelay, float sampleRate)
      : m_fftFrame(std::move(fftFrame)),
        m_frameDelay(frameDelay),
        m_sampleRate(sampleRate) {}

  UniquePtr<FFTBlock> m_fftFrame;
  float m_frameDelay;
  float m_sampleRate;
};

typedef nsTArray<nsAutoRef<HRTFKernel> > HRTFKernelList;

}  

template <>
class nsAutoRefTraits<WebCore::HRTFKernel>
    : public nsPointerRefTraits<WebCore::HRTFKernel> {
 public:
  static void Release(WebCore::HRTFKernel* ptr) { delete (ptr); }
};

namespace WebCore {

inline nsReturnRef<HRTFKernel> HRTFKernel::create(float* impulseResponse,
                                                  size_t length,
                                                  float sampleRate) {
  return nsReturnRef<HRTFKernel>(
      new HRTFKernel(impulseResponse, length, sampleRate));
}

inline nsReturnRef<HRTFKernel> HRTFKernel::create(UniquePtr<FFTBlock> fftFrame,
                                                  float frameDelay,
                                                  float sampleRate) {
  return nsReturnRef<HRTFKernel>(
      new HRTFKernel(std::move(fftFrame), frameDelay, sampleRate));
}

}  

#endif  // HRTFKernel_h
