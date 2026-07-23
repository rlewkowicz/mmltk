/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef XPCOM_THREADS_EVENTTARGETCAPABILITY_H_
#define XPCOM_THREADS_EVENTTARGETCAPABILITY_H_

#include "mozilla/ThreadSafety.h"
#include "nsIEventTarget.h"

namespace mozilla {


template <typename T>
class MOZ_CAPABILITY("event target") EventTargetCapability final {
  static_assert(std::is_base_of_v<nsIEventTarget, T>,
                "T must derive from nsIEventTarget");

 public:
  explicit EventTargetCapability(T* aTarget) : mTarget(aTarget) {
    MOZ_ASSERT(mTarget, "mTarget should be non-null");
  }
  ~EventTargetCapability() = default;

  EventTargetCapability(const EventTargetCapability&) = default;
  EventTargetCapability(EventTargetCapability&&) = default;
  EventTargetCapability& operator=(const EventTargetCapability&) = default;
  EventTargetCapability& operator=(EventTargetCapability&&) = default;

  void AssertOnCurrentThread() const MOZ_ASSERT_CAPABILITY(this) {
    MOZ_ASSERT(IsOnCurrentThread());
  }

  bool IsOnCurrentThread() const { return mTarget->IsOnCurrentThread(); }

  T* GetEventTarget() const { return mTarget; }

  nsresult Dispatch(
      already_AddRefed<nsIRunnable> aRunnable,
      nsIEventTarget::DispatchFlags aFlags = NS_DISPATCH_NORMAL) const {
    return mTarget->Dispatch(std::move(aRunnable), aFlags);
  }

 private:
  RefPtr<T> mTarget;
};

}  

#endif  // XPCOM_THREADS_EVENTTARGETCAPABILITY_H_
