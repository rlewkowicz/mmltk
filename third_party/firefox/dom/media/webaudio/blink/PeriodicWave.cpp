/*
 * Copyright (C) 2012 Google Inc. All rights reserved.
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

#include "PeriodicWave.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "mozilla/FFTBlock.h"

const unsigned MinPeriodicWaveSize = 4096;  
const unsigned MaxPeriodicWaveSize = 8192;  
const float CentsPerRange = 1200 / 3;       

using namespace mozilla;
using mozilla::dom::OscillatorType;

namespace WebCore {

already_AddRefed<PeriodicWave> PeriodicWave::create(float sampleRate,
                                                    const float* real,
                                                    const float* imag,
                                                    size_t numberOfComponents,
                                                    bool disableNormalization) {
  bool isGood = real && imag && numberOfComponents > 0;
  MOZ_ASSERT(isGood);
  if (isGood) {
    RefPtr<PeriodicWave> periodicWave =
        new PeriodicWave(sampleRate, numberOfComponents, disableNormalization);

    size_t halfSize = periodicWave->m_periodicWaveSize / 2;
    numberOfComponents = std::min(numberOfComponents, halfSize);
    periodicWave->m_numberOfComponents = numberOfComponents;
    periodicWave->m_realComponents =
        MakeUnique<AudioFloatArray>(numberOfComponents);
    periodicWave->m_imagComponents =
        MakeUnique<AudioFloatArray>(numberOfComponents);
    memcpy(periodicWave->m_realComponents->Elements(), real,
           numberOfComponents * sizeof(float));
    memcpy(periodicWave->m_imagComponents->Elements(), imag,
           numberOfComponents * sizeof(float));

    return periodicWave.forget();
  }
  return nullptr;
}

already_AddRefed<PeriodicWave> PeriodicWave::createSine(float sampleRate) {
  RefPtr<PeriodicWave> periodicWave =
      new PeriodicWave(sampleRate, MinPeriodicWaveSize, false);
  periodicWave->generateBasicWaveform(OscillatorType::Sine);
  return periodicWave.forget();
}

already_AddRefed<PeriodicWave> PeriodicWave::createSquare(float sampleRate) {
  RefPtr<PeriodicWave> periodicWave =
      new PeriodicWave(sampleRate, MinPeriodicWaveSize, false);
  periodicWave->generateBasicWaveform(OscillatorType::Square);
  return periodicWave.forget();
}

already_AddRefed<PeriodicWave> PeriodicWave::createSawtooth(float sampleRate) {
  RefPtr<PeriodicWave> periodicWave =
      new PeriodicWave(sampleRate, MinPeriodicWaveSize, false);
  periodicWave->generateBasicWaveform(OscillatorType::Sawtooth);
  return periodicWave.forget();
}

already_AddRefed<PeriodicWave> PeriodicWave::createTriangle(float sampleRate) {
  RefPtr<PeriodicWave> periodicWave =
      new PeriodicWave(sampleRate, MinPeriodicWaveSize, false);
  periodicWave->generateBasicWaveform(OscillatorType::Triangle);
  return periodicWave.forget();
}

PeriodicWave::PeriodicWave(float sampleRate, size_t numberOfComponents,
                           bool disableNormalization)
    : m_sampleRate(sampleRate),
      m_centsPerRange(CentsPerRange),
      m_maxPartialsInBandLimitedTable(0),
      m_normalizationScale(1.0f),
      m_disableNormalization(disableNormalization) {
  float nyquist = 0.5 * m_sampleRate;

  if (numberOfComponents <= MinPeriodicWaveSize) {
    m_periodicWaveSize = MinPeriodicWaveSize;
  } else {
    unsigned npow2 = fdlibm_exp2f(floorf(
        fdlibm_logf(numberOfComponents - 1.0) / fdlibm_logf(2.0f) + 1.0f));
    m_periodicWaveSize = std::min(MaxPeriodicWaveSize, npow2);
  }

  m_numberOfRanges =
      (unsigned)(3.0f * fdlibm_logf(m_periodicWaveSize) / fdlibm_logf(2.0f));
  m_bandLimitedTables.SetLength(m_numberOfRanges);
  m_lowestFundamentalFrequency = nyquist / maxNumberOfPartials();
  m_rateScale = m_periodicWaveSize / m_sampleRate;
}

size_t PeriodicWave::sizeOfIncludingThis(
    mozilla::MallocSizeOf aMallocSizeOf) const {
  size_t amount = aMallocSizeOf(this);

  amount += m_bandLimitedTables.ShallowSizeOfExcludingThis(aMallocSizeOf);
  for (size_t i = 0; i < m_bandLimitedTables.Length(); i++) {
    if (m_bandLimitedTables[i]) {
      amount +=
          m_bandLimitedTables[i]->ShallowSizeOfIncludingThis(aMallocSizeOf);
    }
  }

  return amount;
}

void PeriodicWave::waveDataForFundamentalFrequency(
    float fundamentalFrequency, float*& lowerWaveData, float*& higherWaveData,
    float& tableInterpolationFactor) {
  fundamentalFrequency = fabsf(fundamentalFrequency);

  unsigned numberOfPartials = numberOfPartialsForRange(0);
  float nyquist = 0.5 * m_sampleRate;
  if (fundamentalFrequency != 0.0) {
    numberOfPartials =
        std::min(numberOfPartials, (unsigned)(nyquist / fundamentalFrequency));
  }
  if (numberOfPartials > m_maxPartialsInBandLimitedTable) {
    for (unsigned rangeIndex = 0; rangeIndex < m_numberOfRanges; ++rangeIndex) {
      m_bandLimitedTables[rangeIndex] = nullptr;
    }

    createBandLimitedTables(fundamentalFrequency, 0);
    m_maxPartialsInBandLimitedTable = numberOfPartials;
  }

  float ratio = fundamentalFrequency > 0
                    ? fundamentalFrequency / m_lowestFundamentalFrequency
                    : 0.5;
  float centsAboveLowestFrequency =
      fdlibm_logf(ratio) / fdlibm_logf(2.0f) * 1200;

  float pitchRange = 1 + centsAboveLowestFrequency / m_centsPerRange;

  pitchRange = std::max(pitchRange, 0.0f);
  pitchRange = std::min(pitchRange, static_cast<float>(m_numberOfRanges - 1));

  unsigned rangeIndex1 = static_cast<unsigned>(pitchRange);
  unsigned rangeIndex2 =
      rangeIndex1 < m_numberOfRanges - 1 ? rangeIndex1 + 1 : rangeIndex1;

  if (!m_bandLimitedTables[rangeIndex1].get())
    createBandLimitedTables(fundamentalFrequency, rangeIndex1);

  if (!m_bandLimitedTables[rangeIndex2].get())
    createBandLimitedTables(fundamentalFrequency, rangeIndex2);

  lowerWaveData = m_bandLimitedTables[rangeIndex2]->Elements();
  higherWaveData = m_bandLimitedTables[rangeIndex1]->Elements();

  tableInterpolationFactor = rangeIndex2 - pitchRange;
}

unsigned PeriodicWave::maxNumberOfPartials() const {
  return m_periodicWaveSize / 2;
}

unsigned PeriodicWave::numberOfPartialsForRange(unsigned rangeIndex) const {
  float centsToCull = rangeIndex * m_centsPerRange;

  float cullingScale = fdlibm_exp2f(-centsToCull / 1200);

  unsigned numberOfPartials = cullingScale * maxNumberOfPartials();

  return numberOfPartials;
}

void PeriodicWave::createBandLimitedTables(float fundamentalFrequency,
                                           unsigned rangeIndex) {
  unsigned fftSize = m_periodicWaveSize;
  unsigned i;

  const float* realData = m_realComponents->Elements();
  const float* imagData = m_imagComponents->Elements();

  FFTBlock frame(fftSize);

  unsigned numberOfPartials = numberOfPartialsForRange(rangeIndex);
  numberOfPartials = std::min(numberOfPartials, m_numberOfComponents - 1);

  float nyquist = 0.5 * m_sampleRate;
  if (fundamentalFrequency != 0.0) {
    numberOfPartials =
        std::min(numberOfPartials, (unsigned)(nyquist / fundamentalFrequency));
  }

  for (i = 0; i < numberOfPartials + 1; ++i) {
    frame.RealData(i) = realData[i];
    frame.ImagData(i) = -imagData[i];
  }

  frame.RealData(0) = 0;
  frame.ImagData(0) = 0;

  m_bandLimitedTables[rangeIndex] =
      MakeUnique<AlignedAudioFloatArray>(m_periodicWaveSize);

  float* data = m_bandLimitedTables[rangeIndex]->Elements();
  frame.GetInverse(data);

  if (m_disableNormalization) {
    m_normalizationScale = 0.5;
  } else if (!rangeIndex) {
    float maxValue;
    maxValue = AudioBufferPeakValue(data, m_periodicWaveSize);

    if (maxValue) m_normalizationScale = 1.0f / maxValue;
  }

  AudioBufferInPlaceScale(data, m_normalizationScale, m_periodicWaveSize);
}

void PeriodicWave::generateBasicWaveform(OscillatorType shape) {
  const float piFloat = std::numbers::pi_v<float>;
  unsigned fftSize = periodicWaveSize();
  unsigned halfSize = fftSize / 2;

  m_numberOfComponents = halfSize;
  m_realComponents = MakeUnique<AudioFloatArray>(halfSize);
  m_imagComponents = MakeUnique<AudioFloatArray>(halfSize);
  float* realP = m_realComponents->Elements();
  float* imagP = m_imagComponents->Elements();

  realP[0] = 0;
  imagP[0] = 0;

  for (unsigned n = 1; n < halfSize; ++n) {
    float omega = 2 * piFloat * n;
    float invOmega = 1 / omega;

    float a;  
    float b;  

    switch (shape) {
      case OscillatorType::Sine:
        a = 0;
        b = (n == 1) ? 1 : 0;
        break;
      case OscillatorType::Square:
        a = 0;
        b = invOmega * ((n & 1) ? 2 : 0);
        break;
      case OscillatorType::Sawtooth:
        a = 0;
        b = -invOmega * fdlibm_cos(0.5 * omega);
        break;
      case OscillatorType::Triangle:
        a = 0;
        if (n & 1) {
          b = 2 * (2 / (n * piFloat) * 2 / (n * piFloat)) *
              ((((n - 1) >> 1) & 1) ? -1 : 1);
        } else {
          b = 0;
        }
        break;
      default:
        MOZ_ASSERT_UNREACHABLE("invalid oscillator type");
        a = 0;
        b = 0;
        break;
    }

    realP[n] = a;
    imagP[n] = b;
  }
}

}  
