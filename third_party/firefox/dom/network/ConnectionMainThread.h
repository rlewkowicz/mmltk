/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_network_ConnectionMainThread_h
#define mozilla_dom_network_ConnectionMainThread_h

#include "Connection.h"
#include "mozilla/Hal.h"
#include "mozilla/Observer.h"

namespace mozilla::dom::network {

class ConnectionMainThread final : public Connection,
                                   public hal::NetworkObserver {
 public:
  explicit ConnectionMainThread(nsPIDOMWindowInner* aWindow,
                                bool aShouldResistFingerprinting);

  void Notify(const hal::NetworkInformation& aNetworkInfo) override;

 private:
  ~ConnectionMainThread();

  virtual void ShutdownInternal() override;

  void UpdateFromNetworkInfo(const hal::NetworkInformation& aNetworkInfo,
                             bool aNotify);
};

}  

#endif  // mozilla_dom_network_ConnectionMainThread_h
