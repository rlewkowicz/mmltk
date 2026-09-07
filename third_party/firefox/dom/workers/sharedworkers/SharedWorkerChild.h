/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_dom_SharedWorkerChild_h
#define mozilla_dom_dom_SharedWorkerChild_h

#include "mozilla/dom/PSharedWorkerChild.h"
#include "nsISupportsImpl.h"

namespace mozilla::dom {

class SharedWorker;

class SharedWorkerChild final : public mozilla::dom::PSharedWorkerChild {
  friend class PSharedWorkerChild;

 public:
  NS_INLINE_DECL_REFCOUNTING(SharedWorkerChild)

  SharedWorkerChild();

  void SetParent(SharedWorker* aSharedWorker) { mParent = aSharedWorker; }

  void SendClose();

  void SendSuspend();

  void SendResume();

  void SendFreeze();

  void SendThaw();

  void SendSetLocaleOverride(const nsACString& aLanguageOverride,
                             const nsTArray<nsString>& aLanguages);

  void SendUpdateTimezoneOverride(const nsAString& aTimezoneOverride);

 private:
  ~SharedWorkerChild();

  mozilla::ipc::IPCResult RecvError(const ErrorValue& aValue);

  mozilla::ipc::IPCResult RecvNotifyLock(bool aCreated);

  mozilla::ipc::IPCResult RecvNotifyWebTransport(bool aCreated);

  mozilla::ipc::IPCResult RecvTerminate();

  void ActorDestroy(ActorDestroyReason aWhy) override;

  SharedWorker* MOZ_NON_OWNING_REF mParent;
  bool mActive;
};

}  

#endif  // mozilla_dom_dom_SharedWorkerChild_h
