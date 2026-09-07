/*
 * Copyright (C) 2010, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef HRTFPanner_h
#define HRTFPanner_h

#include "DelayBuffer.h"
#include "FFTConvolver.h"
#include "mozilla/MemoryReporting.h"

namespace mozilla {
class AudioBlock;
}  

namespace WebCore {

typedef nsTArray<float> AudioFloatArray;

class HRTFDatabaseLoader;

using mozilla::AudioBlock;

class HRTFPanner {
 public:
  HRTFPanner(float sampleRate,
             already_AddRefed<HRTFDatabaseLoader> databaseLoader);
  ~HRTFPanner();

  void pan(double azimuth, double elevation, const AudioBlock* inputBus,
           AudioBlock* outputBus);
  void reset();

  size_t fftSize() const { return m_convolverL1.fftSize(); }

  float sampleRate() const { return m_sampleRate; }

  int maxTailFrames() const;

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;

  HRTFDatabaseLoader* DatabaseLoader() const { return m_databaseLoader; }

 private:
  int calculateDesiredAzimuthIndexAndBlend(double azimuth,
                                           double& azimuthBlend);

  const RefPtr<HRTFDatabaseLoader> m_databaseLoader;

  float m_sampleRate;


  enum CrossfadeSelection { CrossfadeSelection1, CrossfadeSelection2 };

  CrossfadeSelection m_crossfadeSelection;

  int m_azimuthIndex1;
  double m_elevation1;

  int m_azimuthIndex2;
  double m_elevation2;

  float m_crossfadeX;

  float m_crossfadeIncr;

  FFTConvolver m_convolverL1;
  FFTConvolver m_convolverR1;
  FFTConvolver m_convolverL2;
  FFTConvolver m_convolverR2;

  mozilla::DelayBuffer m_delayLine;

  AudioFloatArray m_tempL1;
  AudioFloatArray m_tempR1;
  AudioFloatArray m_tempL2;
  AudioFloatArray m_tempR2;
};

}  

#endif  // HRTFPanner_h
