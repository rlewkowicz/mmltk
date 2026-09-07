/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(include_gfx_ipc_UiCompositorControllerChild_h)
#define include_gfx_ipc_UiCompositorControllerChild_h

#include "mozilla/layers/PUiCompositorControllerChild.h"

#include "mozilla/gfx/2D.h"
#include "mozilla/Maybe.h"
#include "mozilla/layers/UiCompositorControllerParent.h"
#include "mozilla/RefPtr.h"
#include "nsThread.h"

class nsIWidget;

namespace mozilla {
namespace layers {

class AndroidHardwareBuffer;

class UiCompositorControllerChild final
    : protected PUiCompositorControllerChild {
  friend class PUiCompositorControllerChild;

 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(UiCompositorControllerChild, final)

  static RefPtr<UiCompositorControllerChild> CreateForSameProcess(
      const LayersId& aRootLayerTreeId, nsIWidget* aWidget);
  static RefPtr<UiCompositorControllerChild> CreateForGPUProcess(
      const uint64_t& aProcessToken,
      Endpoint<PUiCompositorControllerChild>&& aEndpoint, nsIWidget* aWidget);

  bool Pause();
  bool Resume();
  bool ResumeAndResize(const int32_t& aX, const int32_t& aY,
                       const int32_t& aHeight, const int32_t& aWidth);
  bool InvalidateAndRender();
  bool SetMaxToolbarHeight(const int32_t& aHeight);
  bool SetFixedBottomOffset(int32_t aOffset);
  bool ToolbarAnimatorMessageFromUI(const int32_t& aMessage);
  bool SetDefaultClearColor(const uint32_t& aColor);
  bool EnableLayerUpdateNotifications(const bool& aEnable);

  void Destroy();


 protected:
  void ActorDestroy(ActorDestroyReason aWhy) override;
  void ProcessingError(Result aCode, const char* aReason) override;
  void HandleFatalError(const char* aMsg) override;
  mozilla::ipc::IPCResult RecvToolbarAnimatorMessageFromCompositor(
      const int32_t& aMessage);
  mozilla::ipc::IPCResult RecvNotifyCompositorScrollUpdate(
      const CompositorScrollUpdate& aUpdate);
  mozilla::ipc::IPCResult RecvScreenPixels(
      uint64_t aRequestId, bool aSuccess,
      Maybe<ipc::FileDescriptor>&& aAcquireFence);

 private:
  explicit UiCompositorControllerChild(const uint64_t& aProcessToken,
                                       nsIWidget* aWidget);
  virtual ~UiCompositorControllerChild();
  void OpenForSameProcess();
  void OpenForGPUProcess(Endpoint<PUiCompositorControllerChild>&& aEndpoint);
  void SendCachedValues();

  void SetReplyTimeout();
  bool ShouldContinueFromReplyTimeout() override;

  bool mIsOpen;
  uint64_t mProcessToken;
  Maybe<gfx::IntRect> mResize;
  Maybe<int32_t> mMaxToolbarHeight;
  Maybe<uint32_t> mDefaultClearColor;
  Maybe<bool> mLayerUpdateEnabled;
  RefPtr<nsIWidget> mWidget;


  RefPtr<UiCompositorControllerParent> mParent;

};

}  
}  

#endif
