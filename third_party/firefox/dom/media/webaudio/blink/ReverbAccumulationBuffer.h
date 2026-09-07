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

#ifndef ReverbAccumulationBuffer_h
#define ReverbAccumulationBuffer_h

#include "AlignedTArray.h"
#include "mozilla/MemoryReporting.h"

namespace WebCore {

class ReverbAccumulationBuffer {
 public:
  ReverbAccumulationBuffer();
  bool allocate(size_t length);

  void readAndClear(float* destination, size_t numberOfFrames);

  void accumulate(const float* source, size_t numberOfFrames, size_t* readIndex,
                  size_t delayFrames);

  size_t readIndex() const { return m_readIndex; }

  size_t readTimeFrame() const { return m_readTimeFrame; }

  void reset();

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf) const {
    return m_buffer.ShallowSizeOfExcludingThis(aMallocSizeOf);
  }

 private:
  AlignedTArray<float, 16> m_buffer;
  size_t m_readIndex;
  size_t m_readTimeFrame;  
};

}  

#endif  // ReverbAccumulationBuffer_h
