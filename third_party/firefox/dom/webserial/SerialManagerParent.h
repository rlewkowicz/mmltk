/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SerialManagerParent_h
#define mozilla_dom_SerialManagerParent_h

#include "mozilla/Mutex.h"
#include "mozilla/dom/PSerialManagerParent.h"
#include "mozilla/dom/SerialPlatformService.h"
#include "mozilla/ipc/Endpoint.h"
#include "nsIObserver.h"

namespace mozilla::dom {

class SerialPermissionRequest;
class SerialPortParent;

class SerialDeviceChangeProxy final : public SerialDeviceChangeObserver,
                                      public nsIObserver {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIOBSERVER

  explicit SerialDeviceChangeProxy(
      uint64_t aBrowserId, RefPtr<SerialPlatformService> aPlatformService);

  void AddPortActor(SerialPortParent* aActor);
  void RemovePortActor(SerialPortParent* aActor);

  void RevokeAllPorts();

  void OnPortConnected(const IPCSerialPortInfo& aPortInfo) override;
  void OnPortDisconnected(const nsAString& aPortId) override;

 private:
  ~SerialDeviceChangeProxy();
  nsTArray<RefPtr<SerialPortParent>> ActorsById(const nsAString& aPortId);

  Mutex mMutex{"SerialDeviceChangeProxy"};

  nsTArray<RefPtr<SerialPortParent>> mPortActors MOZ_GUARDED_BY(mMutex);
  const uint64_t mBrowserId;
  const RefPtr<SerialPlatformService> mPlatformService;
};

class SerialManagerParent final : public PSerialManagerParent {
 public:
  NS_INLINE_DECL_REFCOUNTING(SerialManagerParent, override)

  SerialManagerParent();

  void Init(uint64_t aBrowserId);

  void ActorDestroy(ActorDestroyReason aWhy) override;

  mozilla::ipc::IPCResult RecvRequestPort(
      nsTArray<IPCSerialPortFilter>&& aFilters, RequestPortResolver&& aResolver);

 private:
  ~SerialManagerParent();

  void StartChooserRequest(nsTArray<IPCSerialPortInfo>&& aPorts,
                           RequestPortResolver&& aResolver);

  mozilla::ipc::Endpoint<PSerialPortChild> CreateAndBindPortActor(
      const nsAString& aPortId);

  uint64_t mBrowserId = 0;
  RefPtr<SerialPlatformService> mPlatformService;
  RefPtr<SerialDeviceChangeProxy> mProxy;

  bool mChooserRequestInFlight = false;
};

}  

#endif  // mozilla_dom_SerialManagerParent_h
