/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SOUNDTOUCH_ADAPTER_H_
#define SOUNDTOUCH_ADAPTER_H_

#include "AudioSampleFormat.h"
#include "mozilla/Types.h"
#include "soundtouch/SoundTouch.h"

namespace mozilla {

class MOZ_EXPORT SoundTouchAdapter final {
 public:
  SoundTouchAdapter() = default;
  bool Init() { return true; }

  void setSampleRate(uint aRate) { mProcessor.setSampleRate(aRate); }
  void setChannels(uint aChannels);
  void setPitch(double aPitch) { mProcessor.setPitch(aPitch); }
  void setSetting(int aSettingId, int aValue) {
    mProcessor.setSetting(aSettingId, aValue);
  }
  void setTempo(double aTempo) { mProcessor.setTempo(aTempo); }
  void setRate(double aRate) { mProcessor.setRate(aRate); }
  uint numChannels() const { return mChannels; }
  uint numSamples() const { return mProcessor.numSamples(); }
  uint numUnprocessedSamples() const {
    return mProcessor.numUnprocessedSamples();
  }
  void putSamples(const AudioDataValue* aSamples, uint aNumSamples);
  uint receiveSamples(AudioDataValue* aOutput, uint aMaxSamples);
  void flush() { mProcessor.flush(); }

 private:
  uint mChannels = 0;
  soundtouch::SoundTouch mProcessor;
};

}  

#endif
