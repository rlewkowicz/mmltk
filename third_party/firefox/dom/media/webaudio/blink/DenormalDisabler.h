/*
 * Copyright (C) 2011, Google Inc. All rights reserved.
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

#if !defined(DenormalDisabler_h)
#define DenormalDisabler_h

#include <float.h>

#include <cmath>
#include <cstring>

namespace WebCore {




#if defined(__GNUC__) && defined(__SSE__)
#  define HAVE_DENORMAL 1
#endif

#if defined(__arm__) || defined(__aarch64__)
#  define HAVE_DENORMAL 1
#endif

#if defined(HAVE_DENORMAL)
class DenormalDisabler {
 public:
  DenormalDisabler() : m_savedCSR(0) { disableDenormals(); }

  ~DenormalDisabler() { restoreState(); }

  static inline float flushDenormalFloatToZero(float f) { return f; }

 private:
  unsigned m_savedCSR;

#if defined(__GNUC__) && defined(__SSE__)
  static inline bool isDAZSupported() {
#if defined(__x86_64__)
    return true;
#else
    static bool s_isInited = false;
    static bool s_isSupported = false;
    if (s_isInited) {
      return s_isSupported;
    }

    struct fxsaveResult {
      uint8_t before[28];
      uint32_t CSRMask;
      uint8_t after[480];
    } __attribute__((aligned(16)));

    fxsaveResult registerData;
    memset(&registerData, 0, sizeof(fxsaveResult));
    asm volatile("fxsave %0" : "=m"(registerData));
    s_isSupported = registerData.CSRMask & 0x0040;
    s_isInited = true;
    return s_isSupported;
#endif
  }

  inline void disableDenormals() {
    m_savedCSR = getCSR();
    setCSR(m_savedCSR | (isDAZSupported() ? 0x8040 : 0x8000));
  }

  inline void restoreState() { setCSR(m_savedCSR); }

  inline int getCSR() {
    int result;
    asm volatile("stmxcsr %0" : "=m"(result));
    return result;
  }

  inline void setCSR(int a) {
    int temp = a;
    asm volatile("ldmxcsr %0" : : "m"(temp));
  }

#elif defined(__arm__) || defined(__aarch64__)
  inline void disableDenormals() {
    m_savedCSR = getStatusWord();
    setStatusWord(m_savedCSR | (1 << 24));
  }

  inline void restoreState() { setStatusWord(m_savedCSR); }

  inline int getStatusWord() {
    int result;
#if defined(__aarch64__)
    asm volatile("mrs %x[result], FPCR" : [result] "=r"(result));
#else
    asm volatile("vmrs %[result], FPSCR" : [result] "=r"(result));
#endif
    return result;
  }

  inline void setStatusWord(int a) {
#if defined(__aarch64__)
    asm volatile("msr FPCR, %x[src]" : : [src] "r"(a));
#else
    asm volatile("vmsr FPSCR, %[src]" : : [src] "r"(a));
#endif
  }

#endif
};

#else
class DenormalDisabler {
 public:
  DenormalDisabler() {}

  static inline float flushDenormalFloatToZero(float f) {
    return (fabs(f) < FLT_MIN) ? 0.0f : f;
  }
};

#endif

}  
#endif
