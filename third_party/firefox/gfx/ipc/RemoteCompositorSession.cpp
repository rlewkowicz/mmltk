/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RemoteCompositorSession.h"
#include "mozilla/VsyncDispatcher.h"
#include "mozilla/gfx/GPUProcessManager.h"
#include "mozilla/layers/APZChild.h"
#include "mozilla/layers/APZCTreeManagerChild.h"
#include "mozilla/layers/CompositorBridgeChild.h"
#include "mozilla/layers/GeckoContentController.h"
#include "nsIWidget.h"

namespace mozilla {
namespace layers {

using namespace gfx;
using namespace widget;

RemoteCompositorSession::RemoteCompositorSession(
    nsIWidget* aWidget, CompositorBridgeChild* aChild,
    CompositorWidgetDelegate* aWidgetDelegate,
    RefPtr<APZCTreeManagerChild>&& aAPZ, const LayersId& aRootLayerTreeId)
    : CompositorSession(aWidget, aWidgetDelegate, aChild, aRootLayerTreeId),
      mAPZ(std::move(aAPZ)) {
  GPUProcessManager::Get()->RegisterRemoteProcessSession(this);
  if (mAPZ) {
    mAPZ->SetCompositorSession(this);
  }
}

RemoteCompositorSession::~RemoteCompositorSession() {
  MOZ_ASSERT(!mCompositorBridgeChild);
}

void RemoteCompositorSession::NotifySessionLost() {
  RefPtr<nsIWidget> widget(mWidget);
  widget->NotifyCompositorSessionLost(this);
}

CompositorBridgeParent* RemoteCompositorSession::GetInProcessBridge() const {
  return nullptr;
}

void RemoteCompositorSession::SetContentController(
    GeckoContentController* aController) {
  mContentController = aController;
  mCompositorBridgeChild->SendPAPZConstructor(new APZChild(aController),
                                              LayersId{0});
}

GeckoContentController* RemoteCompositorSession::GetContentController() {
  return mContentController.get();
}

nsIWidget* RemoteCompositorSession::GetWidget() const { return mWidget; }

RefPtr<IAPZCTreeManager> RemoteCompositorSession::GetAPZCTreeManager() const {
  return mAPZ;
}

void RemoteCompositorSession::Shutdown() {
  mContentController = nullptr;
  if (mAPZ) {
    mAPZ->SetCompositorSession(nullptr);
    mAPZ->Destroy();
  }
  if (mCompositorBridgeChild) {
    mCompositorBridgeChild->Destroy();
    mCompositorBridgeChild = nullptr;
  }
  mCompositorWidgetDelegate = nullptr;
  mWidget = nullptr;
  GPUProcessManager::Get()->UnregisterRemoteProcessSession(this);
}

}  
}  
