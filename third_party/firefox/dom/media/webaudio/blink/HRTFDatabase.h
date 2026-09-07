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

#ifndef HRTFDatabase_h
#define HRTFDatabase_h

#include "HRTFElevation.h"
#include "mozilla/MemoryReporting.h"
#include "nsAutoRef.h"
#include "nsTArray.h"

namespace WebCore {

class HRTFKernel;

class HRTFDatabase {
 public:
  HRTFDatabase(const HRTFDatabase& other) = delete;
  void operator=(const HRTFDatabase& other) = delete;

  static nsReturnRef<HRTFDatabase> create(float sampleRate);

  // clang-format off
  // clang-format on
  void getKernelsFromAzimuthElevation(double azimuthBlend,
                                      unsigned azimuthIndex,
                                      double elevationAngle,
                                      HRTFKernel*& kernelL,
                                      HRTFKernel*& kernelR, double& frameDelayL,
                                      double& frameDelayR);

  static unsigned numberOfAzimuths() {
    return HRTFElevation::NumberOfTotalAzimuths;
  }

  float sampleRate() const { return m_sampleRate; }

  static const unsigned NumberOfRawElevations;

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;

 private:
  explicit HRTFDatabase(float sampleRate);

  static const int MinElevation;
  static const int MaxElevation;
  static const unsigned RawElevationAngleSpacing;

  static const unsigned InterpolationFactor;

  static const unsigned NumberOfTotalElevations;

  static unsigned indexFromElevationAngle(double);

  nsTArray<nsAutoRef<HRTFElevation> > m_elevations;
  float m_sampleRate;
};

}  

template <>
class nsAutoRefTraits<WebCore::HRTFDatabase>
    : public nsPointerRefTraits<WebCore::HRTFDatabase> {
 public:
  static void Release(WebCore::HRTFDatabase* ptr) { delete (ptr); }
};

#endif  // HRTFDatabase_h
