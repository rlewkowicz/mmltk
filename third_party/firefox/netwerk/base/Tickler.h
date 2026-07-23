/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_net_Tickler_h)
#define mozilla_net_Tickler_h



#include "nsISupports.h"
#include <stdint.h>

#if defined(MOZ_USE_WIFI_TICKLER)
#  include "mozilla/Mutex.h"
#  include "mozilla/TimeStamp.h"
#  include "nsISupports.h"
#  include "nsIThread.h"
#  include "nsITimer.h"
#  include "mozilla/ThreadSafeWeakPtr.h"
#  include "prio.h"

class nsIPrefBranch;
#endif

namespace mozilla {
namespace net {

#if defined(MOZ_USE_WIFI_TICKLER)

#  define NS_TICKLER_IID \
    {0x8f769ed6,         \
     0x207c,             \
     0x4af9,             \
     {0x9f, 0x7e, 0x9e, 0x83, 0x2d, 0xa3, 0x75, 0x4e}}

class Tickler final : public SupportsThreadSafeWeakPtr<Tickler> {
 public:
  MOZ_DECLARE_REFCOUNTED_TYPENAME(Tickler)
  NS_INLINE_DECL_STATIC_IID(NS_TICKLER_IID)

  Tickler();
  void Cancel();
  nsresult Init();
  void SetIPV4Address(uint32_t address);
  void SetIPV4Port(uint16_t port);

  void Tickle();

 private:
  ~Tickler();

  friend class SupportsThreadSafeWeakPtr<Tickler>;
  friend class TicklerTimer;
  Mutex mLock;
  nsCOMPtr<nsIThread> mThread MOZ_GUARDED_BY(mLock);
  nsCOMPtr<nsITimer> mTimer MOZ_GUARDED_BY(mLock);
  nsCOMPtr<nsIPrefBranch> mPrefs MOZ_GUARDED_BY(mLock);

  bool mActive MOZ_GUARDED_BY(mLock);
  bool mCanceled MOZ_GUARDED_BY(mLock);
  bool mEnabled MOZ_GUARDED_BY(mLock);
  uint32_t mDelay MOZ_GUARDED_BY(mLock);
  TimeDuration mDuration MOZ_GUARDED_BY(mLock);
  PRFileDesc* mFD MOZ_GUARDED_BY(mLock);

  TimeStamp mLastTickle MOZ_GUARDED_BY(mLock);
  PRNetAddr mAddr;  

  void PostCheckTickler();
  void MaybeStartTickler();
  void MaybeStartTicklerUnlocked();

  void CheckTickler();
  void StartTickler();
  void StopTickler();
};

#else

class Tickler final : public nsISupports {
  ~Tickler() = default;

 public:
  NS_DECL_THREADSAFE_ISUPPORTS

  Tickler() = default;
  nsresult Init() { return NS_ERROR_NOT_IMPLEMENTED; }
  void Cancel() {}
  void SetIPV4Address(uint32_t) {};
  void SetIPV4Port(uint16_t) {}
  void Tickle() {}
};

#endif

}  
}  

#endif
