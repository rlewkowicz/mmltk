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

#ifndef Biquad_h
#define Biquad_h

#include <complex>

namespace WebCore {

typedef std::complex<double> Complex;


class Biquad {
 public:
  Biquad();
  ~Biquad() = default;

  void process(const float* sourceP, float* destP, size_t framesToProcess);

  void setLowpassParams(double frequency, double resonance);
  void setHighpassParams(double frequency, double resonance);
  void setBandpassParams(double frequency, double Q);
  void setLowShelfParams(double frequency, double dbGain);
  void setHighShelfParams(double frequency, double dbGain);
  void setPeakingParams(double frequency, double Q, double dbGain);
  void setAllpassParams(double frequency, double Q);
  void setNotchParams(double frequency, double Q);

  void setZeroPolePairs(const Complex& zero, const Complex& pole);

  void setAllpassPole(const Complex& pole);

  bool hasTail() const { return m_y1 || m_y2 || m_x1 || m_x2; }

  void reset();

  void getFrequencyResponse(int nFrequencies, const float* frequency,
                            float* magResponse, float* phaseResponse);

 private:
  void setNormalizedCoefficients(double b0, double b1, double b2, double a0,
                                 double a1, double a2);

  double m_b0;
  double m_b1;
  double m_b2;
  double m_a1;
  double m_a2;

  double m_x1;  
  double m_x2;  
  double m_y1;  
  double m_y2;  
};

}  

#endif  // Biquad_h
