/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CAPTURETASK_H
#define CAPTURETASK_H

#include "MediaTrackGraph.h"
#include "MediaTrackListener.h"
#include "PrincipalChangeObserver.h"

namespace mozilla {

namespace dom {
class BlobImpl;
class ImageCapture;
class MediaStreamTrack;
}  

class CaptureTask : public DirectMediaTrackListener,
                    public dom::PrincipalChangeObserver<dom::MediaStreamTrack> {
 public:
  class MediaTrackEventListener;

  void NotifyRealtimeTrackData(MediaTrackGraph* aGraph, TrackTime aTrackOffset,
                               const MediaSegment& aMedia) override;

  void PrincipalChanged(dom::MediaStreamTrack* aMediaStreamTrack) override;


  nsresult TaskComplete(already_AddRefed<dom::BlobImpl> aBlobImpl,
                        nsresult aRv);

  void AttachTrack();

  void DetachTrack();

  explicit CaptureTask(dom::ImageCapture* aImageCapture);

 protected:
  virtual ~CaptureTask() = default;

  void PostTrackEndEvent();

  RefPtr<dom::ImageCapture> mImageCapture;

  RefPtr<MediaTrackEventListener> mEventListener;

  Atomic<bool> mImageGrabbedOrTrackEnd;

  bool mPrincipalChanged;
};

}  

#endif  // CAPTURETASK_H
