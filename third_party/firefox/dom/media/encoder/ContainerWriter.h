/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ContainerWriter_h_
#define ContainerWriter_h_

#include "EncodedFrame.h"
#include "TrackMetadataBase.h"
#include "nsTArray.h"

namespace mozilla {
class ContainerWriter {
 public:
  ContainerWriter() : mInitialized(false), mIsWritingComplete(false) {}
  virtual ~ContainerWriter() {}
  enum {
    CREATE_AUDIO_TRACK = 1 << 0,
    CREATE_VIDEO_TRACK = 1 << 1,
  };
  enum { END_OF_STREAM = 1 << 0 };

  virtual nsresult WriteEncodedTrack(
      const nsTArray<RefPtr<EncodedFrame>>& aData, uint32_t aFlags = 0) = 0;

  virtual nsresult SetMetadata(
      const nsTArray<RefPtr<TrackMetadataBase>>& aMetadata) = 0;

  virtual bool IsWritingComplete() { return mIsWritingComplete; }

  enum { FLUSH_NEEDED = 1 << 0, GET_HEADER = 1 << 1 };

  virtual nsresult GetContainerData(nsTArray<nsTArray<uint8_t>>* aOutputBufs,
                                    uint32_t aFlags = 0) = 0;

 protected:
  bool mInitialized;
  bool mIsWritingComplete;
};

}  

#endif
