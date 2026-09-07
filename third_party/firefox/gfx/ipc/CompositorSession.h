/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(_include_mozilla_gfx_ipc_CompositorSession_h_)
#define _include_mozilla_gfx_ipc_CompositorSession_h_

#include "base/basictypes.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/layers/CompositorTypes.h"
#include "nsISupportsImpl.h"

class nsIWidget;

namespace mozilla {
namespace widget {
class CompositorWidget;
class CompositorWidgetDelegate;
}  
namespace gfx {
class GPUProcessHost;
class GPUProcessManager;
}  
namespace layers {

class GeckoContentController;
class IAPZCTreeManager;
class CompositorBridgeParent;
class CompositorBridgeChild;
class ClientLayerManager;

class CompositorSession {
  friend class gfx::GPUProcessManager;

 protected:
  typedef gfx::GPUProcessHost GPUProcessHost;
  typedef widget::CompositorWidget CompositorWidget;
  typedef widget::CompositorWidgetDelegate CompositorWidgetDelegate;

 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(CompositorSession)

  virtual void Shutdown() = 0;

  virtual CompositorBridgeParent* GetInProcessBridge() const = 0;

  virtual void SetContentController(GeckoContentController* aController) = 0;

  virtual RefPtr<IAPZCTreeManager> GetAPZCTreeManager() const = 0;

  CompositorBridgeChild* GetCompositorBridgeChild();

  CompositorWidgetDelegate* GetCompositorWidgetDelegate() {
    return mCompositorWidgetDelegate;
  }

  LayersId RootLayerTreeId() const { return mRootLayerTreeId; }

 protected:
  CompositorSession(nsIWidget* aWidget, CompositorWidgetDelegate* aDelegate,
                    CompositorBridgeChild* aChild,
                    const LayersId& aRootLayerTreeId);
  virtual ~CompositorSession();

 protected:
  nsIWidget* mWidget;
  CompositorWidgetDelegate* mCompositorWidgetDelegate;
  RefPtr<CompositorBridgeChild> mCompositorBridgeChild;
  LayersId mRootLayerTreeId;
 private:
  DISALLOW_COPY_AND_ASSIGN(CompositorSession);
};

}  
}  

#endif
