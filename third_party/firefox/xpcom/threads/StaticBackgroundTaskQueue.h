/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StaticBackgroundTaskQueue_h
#define mozilla_StaticBackgroundTaskQueue_h

#include "mozilla/Attributes.h"
#include "mozilla/Mutex.h"
#include "mozilla/NeverDestroyed.h"
#include "mozilla/StaticString.h"
#include "nsCOMPtr.h"
#include "nsITargetShutdownTask.h"

namespace mozilla {

class MOZ_STATIC_LOCAL_CLASS StaticBackgroundTaskQueue final {
 public:
  explicit StaticBackgroundTaskQueue(StaticString aName);

  already_AddRefed<nsISerialEventTarget> Get();

 private:
  struct Impl final : public nsITargetShutdownTask {
    explicit Impl(StaticString aName);

    NS_IMETHOD QueryInterface(REFNSIID aIID, void** aInstancePtr) override;
    NS_IMETHOD_(MozExternalRefCountType) AddRef() override { return 2; }
    NS_IMETHOD_(MozExternalRefCountType) Release() override { return 2; }

    void TargetShutdown() override;

    OffTheBooksMutex mMutex;
    nsCOMPtr<nsISerialEventTarget> mQueue MOZ_GUARDED_BY(mMutex);
  };

  NeverDestroyed<Impl> mImpl;
};

}  

#endif  // mozilla_StaticBackgroundTaskQueue_h
