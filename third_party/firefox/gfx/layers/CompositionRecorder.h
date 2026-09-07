/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_CompositionRecorder_h
#define mozilla_layers_CompositionRecorder_h

#include "mozilla/RefPtr.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/layers/PCompositorBridgeTypes.h"
#include "nsISupportsImpl.h"
#include "nsTArray.h"
#include "nsString.h"

namespace mozilla {

namespace gfx {
class DataSourceSurface;
}

namespace layers {

class RecordedFrame {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(RecordedFrame)

  virtual already_AddRefed<gfx::DataSourceSurface> GetSourceSurface() = 0;
  TimeStamp GetTimeStamp() { return mTimeStamp; }

 protected:
  virtual ~RecordedFrame() = default;
  RecordedFrame(const TimeStamp& aTimeStamp) : mTimeStamp(aTimeStamp) {}

 private:
  TimeStamp mTimeStamp;
};

class CompositionRecorder {
 public:
  explicit CompositionRecorder(TimeStamp aRecordingStart);

  void RecordFrame(RecordedFrame* aFrame);

  Maybe<FrameRecording> GetRecording();

 private:
  nsTArray<RefPtr<RecordedFrame>> mRecordedFrames;
  TimeStamp mRecordingStart;
};

}  
}  

#endif  // mozilla_layers_CompositionRecorder_h
