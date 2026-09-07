/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ipc_VsyncMainChild_h
#define mozilla_dom_ipc_VsyncMainChild_h

#include "mozilla/dom/VsyncChild.h"
#include "nsTObserverArray.h"

namespace mozilla {

class VsyncObserver;

namespace dom {

class VsyncMainChild final : public VsyncChild {
  NS_INLINE_DECL_REFCOUNTING(VsyncMainChild, override)

  friend class PVsyncChild;

 public:
  VsyncMainChild();

  void AddChildRefreshTimer(VsyncObserver* aVsyncObserver);
  void RemoveChildRefreshTimer(VsyncObserver* aVsyncObserver);

  TimeDuration GetVsyncRate();

 private:
  ~VsyncMainChild() override;

  mozilla::ipc::IPCResult RecvNotify(const VsyncEvent& aVsync,
                                     const float& aVsyncRate) override;
  void ActorDestroy(ActorDestroyReason aActorDestroyReason) override;

  bool mIsShutdown;
  TimeDuration mVsyncRate;
  nsTObserverArray<VsyncObserver*> mObservers;
};

}  
}  

#endif  // mozilla_dom_ipc_VsyncChild_h
