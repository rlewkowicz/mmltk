/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_CrossProcessSemaphore_h)
#define mozilla_CrossProcessSemaphore_h

#include "base/process.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Maybe.h"

#  include <pthread.h>
#  include <semaphore.h>
#  include "mozilla/ipc/SharedMemoryMapping.h"
#  include "mozilla/Atomics.h"

namespace IPC {
template <typename T>
struct ParamTraits;
}  

namespace mozilla {

template <typename T>
inline bool IsHandleValid(const T& handle) {
  return bool(handle);
}

typedef mozilla::ipc::MutableSharedMemoryHandle CrossProcessSemaphoreHandle;

class CrossProcessSemaphore {
 public:
  static CrossProcessSemaphore* Create(const char* aName,
                                       uint32_t aInitialValue);

  static CrossProcessSemaphore* Create(CrossProcessSemaphoreHandle aHandle);

  ~CrossProcessSemaphore();

  bool Wait(const Maybe<TimeDuration>& aWaitTime = Nothing());

  void Signal();

  CrossProcessSemaphoreHandle CloneHandle();

  void CloseHandle();

 private:
  friend struct IPC::ParamTraits<CrossProcessSemaphore>;

  CrossProcessSemaphore();
  CrossProcessSemaphore(const CrossProcessSemaphore&);
  CrossProcessSemaphore& operator=(const CrossProcessSemaphore&);

  mozilla::ipc::MutableSharedMemoryHandle mHandle;
  mozilla::ipc::SharedMemoryMapping mSharedBuffer;
  sem_t* mSemaphore;
  mozilla::Atomic<int32_t>* mRefCount;
};

}  

#endif
