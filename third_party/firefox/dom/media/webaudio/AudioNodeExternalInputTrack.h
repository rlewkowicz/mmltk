/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_AUDIONODEEXTERNALINPUTTRACK_H_
#define MOZILLA_AUDIONODEEXTERNALINPUTTRACK_H_

#include "AudioNodeTrack.h"
#include "MediaTrackGraph.h"

namespace mozilla {

class AbstractThread;

class AudioNodeExternalInputTrack final : public AudioNodeTrack {
 public:
  static already_AddRefed<AudioNodeExternalInputTrack> Create(
      MediaTrackGraph* aGraph, AudioNodeEngine* aEngine);

 protected:
  AudioNodeExternalInputTrack(AudioNodeEngine* aEngine, TrackRate aSampleRate);
  ~AudioNodeExternalInputTrack();

 public:
  void ProcessInput(GraphTime aFrom, GraphTime aTo, uint32_t aFlags) override;

  AudioNodeExternalInputTrack* AsAudioNodeExternalInputTrack() override {
    return this;
  }

  void SetVolume(float aVolume);

 private:
  bool IsEnabled();

  float mVolume = 1.0;
};

}  

#endif /* MOZILLA_AUDIONODEEXTERNALINPUTTRACK_H_ */
