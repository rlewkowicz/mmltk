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

#ifndef HRTFElevation_h
#define HRTFElevation_h

#include "HRTFKernel.h"
#include "mozilla/MemoryReporting.h"
#include "nsAutoRef.h"

struct SpeexResamplerState_;
typedef struct SpeexResamplerState_ SpeexResamplerState;

namespace WebCore {


class HRTFElevation {
 public:
  HRTFElevation(const HRTFElevation& other) = delete;
  void operator=(const HRTFElevation& other) = delete;

  static nsReturnRef<HRTFElevation> createBuiltin(int elevation,
                                                  float sampleRate);

  static nsReturnRef<HRTFElevation> createByInterpolatingSlices(
      HRTFElevation* hrtfElevation1, HRTFElevation* hrtfElevation2, float x,
      float sampleRate);

  double elevationAngle() const { return m_elevationAngle; }
  unsigned numberOfAzimuths() const { return NumberOfTotalAzimuths; }
  float sampleRate() const { return m_sampleRate; }

  void getKernelsFromAzimuth(double azimuthBlend, unsigned azimuthIndex,
                             HRTFKernel*& kernelL, HRTFKernel*& kernelR,
                             double& frameDelayL, double& frameDelayR);

  static const unsigned NumberOfTotalAzimuths;

  static size_t fftSizeForSampleRate(float sampleRate);

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;

 private:
  HRTFElevation(HRTFKernelList&& kernelListL, int elevation, float sampleRate)
      : m_kernelListL(std::move(kernelListL)),
        m_elevationAngle(elevation),
        m_sampleRate(sampleRate) {}

  const HRTFKernelList& kernelListL() { return m_kernelListL; }

  static nsReturnRef<HRTFKernel> calculateKernelForAzimuthElevation(
      int azimuth, int elevation, SpeexResamplerState* resampler,
      float sampleRate);

  HRTFKernelList m_kernelListL;
  double m_elevationAngle;
  float m_sampleRate;
};

}  

template <>
class nsAutoRefTraits<WebCore::HRTFElevation>
    : public nsPointerRefTraits<WebCore::HRTFElevation> {
 public:
  static void Release(WebCore::HRTFElevation* ptr) { delete (ptr); }
};

#endif  // HRTFElevation_h
