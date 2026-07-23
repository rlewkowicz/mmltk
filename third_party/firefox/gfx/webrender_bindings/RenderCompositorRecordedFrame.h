/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_RENDERCOMPOSITOR_RECORDEDFRAME_H
#define MOZILLA_GFX_RENDERCOMPOSITOR_RECORDEDFRAME_H

#include "mozilla/layers/CompositionRecorder.h"

namespace mozilla {

namespace wr {

class RenderCompositorRecordedFrame final : public layers::RecordedFrame {
 public:
  RenderCompositorRecordedFrame(
      const TimeStamp& aTimeStamp,
      RefPtr<layers::frame_capture::AsyncReadbackBuffer>&& aBuffer)
      : RecordedFrame(aTimeStamp), mBuffer(aBuffer) {}

  virtual already_AddRefed<gfx::DataSourceSurface> GetSourceSurface() override {
    if (mSurface) {
      return do_AddRef(mSurface);
    }

    gfx::IntSize size = mBuffer->Size();
    mSurface = gfx::Factory::CreateDataSourceSurface(
        size, gfx::SurfaceFormat::B8G8R8A8,
         false);

    if (!mBuffer->MapAndCopyInto(mSurface, size)) {
      mSurface = nullptr;
      return nullptr;
    }

    return do_AddRef(mSurface);
  }

 private:
  RefPtr<layers::frame_capture::AsyncReadbackBuffer> mBuffer;
  RefPtr<gfx::DataSourceSurface> mSurface;
};

}  
}  

#endif
