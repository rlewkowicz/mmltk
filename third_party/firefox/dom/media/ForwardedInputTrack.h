/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_FORWARDEDINPUTTRACK_H_
#define MOZILLA_FORWARDEDINPUTTRACK_H_

#include "MediaTrackGraph.h"
#include "MediaTrackListener.h"

namespace mozilla {

class ForwardedInputTrack : public ProcessedMediaTrack {
 public:
  ForwardedInputTrack(TrackRate aSampleRate, MediaSegment::Type aType);

  virtual ForwardedInputTrack* AsForwardedInputTrack() override { return this; }
  friend class DOMMediaStream;

  void AddInput(MediaInputPort* aPort) override;
  void RemoveInput(MediaInputPort* aPort) override;
  void ProcessInput(GraphTime aFrom, GraphTime aTo, uint32_t aFlags) override;

  DisabledTrackMode CombinedDisabledMode() const override;
  void SetDisabledTrackModeImpl(DisabledTrackMode aMode) override;
  void OnInputDisabledModeChanged(DisabledTrackMode aInputMode) override;

  uint32_t NumberOfChannels() const override;

  friend class MediaTrackGraphImpl;

 protected:
  void SetInput(MediaInputPort* aPort);

  void ProcessInputImpl(MediaTrack* aSource, MediaSegment* aSegment,
                        GraphTime aFrom, GraphTime aTo, uint32_t aFlags);

  void AddDirectListenerImpl(
      already_AddRefed<DirectMediaTrackListener> aListener) override;
  void RemoveDirectListenerImpl(DirectMediaTrackListener* aListener) override;
  void RemoveAllDirectListenersImpl() override;

  nsTArray<RefPtr<DirectMediaTrackListener>> mOwnedDirectListeners;

  MediaInputPort* mInputPort = nullptr;

  DisabledTrackMode mInputDisabledMode = DisabledTrackMode::ENABLED;
};

}  

#endif /* MOZILLA_FORWARDEDINPUTTRACK_H_ */
