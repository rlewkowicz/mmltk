/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Timeout.h"
#include "mozilla/dom/TimeoutHandler.h"
#include "mozilla/dom/TimeoutManager.h"

namespace mozilla::dom {

Timeout::Timeout()
    : mTimeoutId(0),
      mFiringId(TimeoutManager::InvalidFiringId),
#ifdef DEBUG
      mFiringIndex(-1),
#endif
      mPopupState(PopupBlocker::openAllowed),
      mReason(Reason::eTimeoutOrInterval),
      mNestingLevel(0),
      mCleared(false),
      mRunning(false),
      mIsInterval(false) {
}

Timeout::~Timeout() { SetTimeoutContainer(nullptr); }

NS_IMPL_CYCLE_COLLECTION_CLASS(Timeout)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(Timeout)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mGlobal)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mScriptHandler)
  if (tmp->isInList()) {
    tmp->remove();
  }
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(Timeout)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mGlobal)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mScriptHandler)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

void Timeout::SetWhenOrTimeRemaining(const TimeStamp& aBaseTime,
                                     const TimeDuration& aDelay) {
  MOZ_DIAGNOSTIC_ASSERT(mGlobal);
  mSubmitTime = aBaseTime;

  if (mGlobal->IsFrozen()) {
    mWhen = TimeStamp();
    mTimeRemaining = aDelay;
    return;
  }

  mWhen = aBaseTime + aDelay;
  mTimeRemaining = TimeDuration();
}

const TimeStamp& Timeout::When() const {
  MOZ_DIAGNOSTIC_ASSERT(!mWhen.IsNull());
  return mWhen;
}

const TimeStamp& Timeout::SubmitTime() const { return mSubmitTime; }

const TimeDuration& Timeout::TimeRemaining() const {
  MOZ_DIAGNOSTIC_ASSERT(mWhen.IsNull());
  return mTimeRemaining;
}

}  
