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

#include "ReverbConvolver.h"

#include "ReverbConvolverStage.h"

using namespace mozilla;

namespace WebCore {

const int InputBufferSize = 8 * 16384;

const size_t RealtimeFrameLimit = 8192 + 4096  
                                  - WEBAUDIO_BLOCK_SIZE;
const size_t MinFFTSize = 256;
const size_t MaxRealtimeFFTSize = 4096;

ReverbConvolver::ReverbConvolver(const float* impulseResponseData,
                                 size_t impulseResponseLength,
                                 size_t maxFFTSize, size_t convolverRenderPhase,
                                 bool useBackgroundThreads,
                                 bool* aAllocationFailure)
    : m_impulseResponseLength(impulseResponseLength),
      m_inputBuffer(InputBufferSize),
      m_backgroundThread("ConvolverWorker"),
      m_backgroundThreadMonitor("ConvolverMonitor"),
      m_useBackgroundThreads(useBackgroundThreads),
      m_wantsToExit(false),
      m_moreInputBuffered(false) {
  *aAllocationFailure = !m_accumulationBuffer.allocate(impulseResponseLength +
                                                       WEBAUDIO_BLOCK_SIZE);
  if (*aAllocationFailure) {
    return;
  }
  bool hasRealtimeConstraint = useBackgroundThreads;

  const float* response = impulseResponseData;
  size_t totalResponseLength = impulseResponseLength;

  size_t reverbTotalLatency = 0;

  size_t stageOffset = 0;
  size_t stagePhase = 0;
  size_t fftSize = MinFFTSize;
  while (stageOffset < totalResponseLength) {
    size_t stageSize = fftSize / 2;

    if (stageSize + stageOffset > totalResponseLength) {
      stageSize = totalResponseLength - stageOffset;
      fftSize = MinFFTSize;
      while (stageSize * 2 > fftSize) {
        fftSize *= 2;
      }
    }

    int renderPhase = convolverRenderPhase + stagePhase;

    UniquePtr<ReverbConvolverStage> stage(new ReverbConvolverStage(
        response, totalResponseLength, reverbTotalLatency, stageOffset,
        stageSize, fftSize, renderPhase, &m_accumulationBuffer));

    bool isBackgroundStage = false;

    if (this->useBackgroundThreads() && stageOffset > RealtimeFrameLimit) {
      m_backgroundStages.AppendElement(std::move(stage));
      isBackgroundStage = true;
    } else
      m_stages.AppendElement(std::move(stage));

    fftSize *= 2;

    stageOffset += stageSize;

    if (hasRealtimeConstraint && !isBackgroundStage &&
        fftSize > MaxRealtimeFFTSize) {
      fftSize = MaxRealtimeFFTSize;
      const uint32_t phaseLookup[] = {14, 0, 10, 4};
      stagePhase = WEBAUDIO_BLOCK_SIZE *
                   phaseLookup[m_stages.Length() % std::size(phaseLookup)];
    } else if (fftSize > maxFFTSize) {
      fftSize = maxFFTSize;
      stagePhase += 5 * WEBAUDIO_BLOCK_SIZE;
    } else if (stageSize > WEBAUDIO_BLOCK_SIZE) {
      stagePhase = stageSize - WEBAUDIO_BLOCK_SIZE;
    }
  }

  if (this->useBackgroundThreads() && m_backgroundStages.Length() > 0) {
    if (!m_backgroundThread.Start()) {
      NS_WARNING("Cannot start convolver thread.");
      return;
    }
    m_backgroundThread.message_loop()->PostTask(NewNonOwningRunnableMethod(
        "WebCore::ReverbConvolver::backgroundThreadEntry", this,
        &ReverbConvolver::backgroundThreadEntry));
  }
}

ReverbConvolver::~ReverbConvolver() {
  if (useBackgroundThreads() && m_backgroundThread.IsRunning()) {
    m_wantsToExit = true;

    {
      MonitorAutoLock locker(m_backgroundThreadMonitor);
      m_moreInputBuffered = true;
      m_backgroundThreadMonitor.Notify();
    }

    m_backgroundThread.Stop();
  }
}

size_t ReverbConvolver::sizeOfIncludingThis(
    mozilla::MallocSizeOf aMallocSizeOf) const {
  size_t amount = aMallocSizeOf(this);
  amount += m_stages.ShallowSizeOfExcludingThis(aMallocSizeOf);
  for (size_t i = 0; i < m_stages.Length(); i++) {
    if (m_stages[i]) {
      amount += m_stages[i]->sizeOfIncludingThis(aMallocSizeOf);
    }
  }

  amount += m_backgroundStages.ShallowSizeOfExcludingThis(aMallocSizeOf);
  for (size_t i = 0; i < m_backgroundStages.Length(); i++) {
    if (m_backgroundStages[i]) {
      amount += m_backgroundStages[i]->sizeOfIncludingThis(aMallocSizeOf);
    }
  }

  amount += m_accumulationBuffer.sizeOfExcludingThis(aMallocSizeOf);
  amount += m_inputBuffer.sizeOfExcludingThis(aMallocSizeOf);

  return amount;
}

void ReverbConvolver::backgroundThreadEntry() {
  while (!m_wantsToExit) {
    m_moreInputBuffered = false;
    {
      MonitorAutoLock locker(m_backgroundThreadMonitor);
      while (!m_moreInputBuffered && !m_wantsToExit)
        m_backgroundThreadMonitor.Wait();
    }

    int writeIndex = m_inputBuffer.writeIndex();

    int readIndex;

    while ((readIndex = m_backgroundStages[0]->inputReadIndex()) !=
           writeIndex) {  
      for (size_t i = 0; i < m_backgroundStages.Length(); ++i)
        m_backgroundStages[i]->processInBackground(this);
    }
  }
}

void ReverbConvolver::process(const float* sourceChannelData,
                              float* destinationChannelData) {
  const float* source = sourceChannelData;
  float* destination = destinationChannelData;
  bool isDataSafe = source && destination;
  MOZ_ASSERT(isDataSafe);
  if (!isDataSafe) return;

  m_inputBuffer.write(source, WEBAUDIO_BLOCK_SIZE);

  for (size_t i = 0; i < m_stages.Length(); ++i) m_stages[i]->process(source);

  m_accumulationBuffer.readAndClear(destination, WEBAUDIO_BLOCK_SIZE);


  if (m_backgroundThreadMonitor.TryLock()) {
    m_moreInputBuffered = true;
    m_backgroundThreadMonitor.Notify();
    m_backgroundThreadMonitor.Unlock();
  }
}

}  
