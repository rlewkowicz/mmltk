/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef mozilla_layers_CompositorThread_h
#define mozilla_layers_CompositorThread_h

#include "base/platform_thread.h"
#include "nsISupportsImpl.h"
#include "nsIThread.h"

class nsIThread;

namespace mozilla {
namespace layers {

class CompositorThreadHolder final {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING_WITH_DELETE_ON_MAIN_THREAD(
      CompositorThreadHolder)

 public:
  CompositorThreadHolder();

  nsIThread* GetCompositorThread() const { return mCompositorThread; }

  bool IsInThread() {
    bool rv = false;
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(mCompositorThread->IsOnCurrentThread(&rv)));
    return rv;
  }

  nsresult Dispatch(
      already_AddRefed<nsIRunnable> event,
      nsIEventTarget::DispatchFlags flags = nsIEventTarget::DISPATCH_NORMAL) {
    return mCompositorThread->Dispatch(std::move(event), flags);
  }

  static CompositorThreadHolder* GetSingleton();

  static bool IsActive() { return !!GetSingleton(); }

  static void Start();

  static void Shutdown();

  static bool IsInCompositorThread();

 private:
  ~CompositorThreadHolder();

  const nsCOMPtr<nsIThread> mCompositorThread;

  static already_AddRefed<nsIThread> CreateCompositorThread();

  friend class CompositorBridgeParent;
};

nsIThread* CompositorThread();

}  
}  

#endif  // mozilla_layers_CompositorThread_h
