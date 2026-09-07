/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_ConditionVariable_h)
#define mozilla_ConditionVariable_h

#include "mozilla/PlatformMutex.h"
#include "mozilla/TimeStamp.h"

#if !defined(__wasi__)
#  include <pthread.h>
#endif

namespace mozilla {

enum class CVStatus { NoTimeout, Timeout };

namespace detail {

class ConditionVariableImpl {
 public:
  MFBT_API ConditionVariableImpl();
  MFBT_API ~ConditionVariableImpl();

  MFBT_API void notify_one();

  MFBT_API void notify_all();

  MFBT_API void wait(MutexImpl& lock);

  MFBT_API CVStatus wait_for(MutexImpl& lock,
                             const mozilla::TimeDuration& rel_time);

 private:
  ConditionVariableImpl(const ConditionVariableImpl&) = delete;
  ConditionVariableImpl& operator=(const ConditionVariableImpl&) = delete;

#if !defined(__wasi__)
  pthread_cond_t mCond;
#endif
};

}  

}  

#endif
