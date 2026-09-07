/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(MOZILLA_LAYERS_RENDEREROGL_H)
#define MOZILLA_LAYERS_RENDEREROGL_H

#include "mozilla/layers/CompositorTypes.h"
#include "mozilla/gfx/Point.h"
#include "mozilla/webrender/RenderThread.h"
#include "mozilla/webrender/WebRenderTypes.h"
#include "mozilla/webrender/webrender_ffi.h"

namespace mozilla {

namespace gfx {
class DrawTarget;
}

namespace gl {
class GLContext;
}

namespace layers {
class AndroidHardwareBuffer;
class CompositorBridgeParent;
class Fence;
class SyncObjectHost;
}  

namespace widget {
class CompositorWidget;
}

namespace wr {

class RenderCompositor;
class RenderTextureHost;

class RendererOGL {
  friend wr::WrExternalImage LockExternalImage(void* aObj,
                                               wr::ExternalImageId aId,
                                               uint8_t aChannelIndex,
                                               wr::ImageRendering);
  friend void UnlockExternalImage(void* aObj, wr::ExternalImageId aId,
                                  uint8_t aChannelIndex);

 public:
  wr::WrExternalImageHandler GetExternalImageHandler();

  void SetFramePublishId(FramePublishId aPublishId);

  void Update();

  RenderedFrameId UpdateAndRender(const Maybe<gfx::IntSize>& aReadbackSize,
                                  const Maybe<wr::ImageFormat>& aReadbackFormat,
                                  const Maybe<Range<uint8_t>>& aReadbackBuffer,
                                  bool* aNeedsYFlip,
                                  const wr::FrameReadyParams& aFrameParams,
                                  RendererStats* aOutStats);

  void WaitForGPU();

  RefPtr<layers::Fence> GetAndResetReleaseFence();

  RenderedFrameId GetLastCompletedFrameId();

  RenderedFrameId UpdateFrameId();

  void SetFrameStartTime(const TimeStamp& aTime);

  void BeginRecording(const TimeStamp& aRecordingStart,
                      wr::PipelineId aPipelineId);
  void MaybeRecordFrame(const WebRenderPipelineInfo* aPipelineInfo);

  Maybe<layers::FrameRecording> EndRecording();


  ~RendererOGL();

  RendererOGL(RefPtr<RenderThread>&& aThread,
              UniquePtr<RenderCompositor> aCompositor, wr::WindowId aWindowId,
              wr::Renderer* aRenderer, layers::CompositorBridgeParent* aBridge);

  void Pause();

  bool Resume();

  bool IsPaused();

  void CheckGraphicsResetStatus(gfx::DeviceResetDetectPlace aPlace,
                                bool aForce);

  layers::SyncObjectHost* GetSyncObject() const;

  layers::CompositorBridgeParent* GetCompositorBridge() { return mBridge; }

  void FlushPipelineInfo();

  RefPtr<const WebRenderPipelineInfo> GetLastPipelineInfo() const {
    return mLastPipelineInfo;
  }

  RenderTextureHost* GetRenderTexture(wr::ExternalImageId aExternalImageId);

  RenderCompositor* GetCompositor() { return mCompositor.get(); }

  void AccumulateMemoryReport(MemoryReport* aReport);

  wr::Renderer* GetRenderer() { return mRenderer; }

  gl::GLContext* gl() const;

  bool EnsureAsyncScreenshot();

 protected:
  bool DidPaintContent(const wr::WebRenderPipelineInfo* aFrameEpochs);


  RefPtr<RenderThread> mThread;
  UniquePtr<RenderCompositor> mCompositor;
  UniquePtr<layers::CompositionRecorder> mCompositionRecorder;  
  wr::Renderer* mRenderer;
  layers::CompositorBridgeParent* mBridge;
  wr::WindowId mWindowId;
  TimeStamp mFrameStartTime;


  wr::PipelineId mRootPipelineId{};

  std::unordered_map<uint64_t, wr::Epoch> mContentPipelineEpochs;

  RefPtr<WebRenderPipelineInfo> mLastPipelineInfo;

  bool mLastFrameDidRasterize = false;

 public:
  bool CheckAndClearDidRasterize();
};

}  
}  

#endif
