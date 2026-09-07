/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_CrossProcessMutex_h)
#define mozilla_CrossProcessMutex_h

#include "base/process.h"
#include "mozilla/Mutex.h"

#  include <pthread.h>
#  include "mozilla/ipc/SharedMemoryMapping.h"
#  include "mozilla/Atomics.h"

namespace IPC {
template <typename T>
struct ParamTraits;
}  

namespace mozilla {
typedef mozilla::ipc::MutableSharedMemoryHandle CrossProcessMutexHandle;

class CrossProcessMutex {
 public:
  explicit CrossProcessMutex(const char* aName);
  explicit CrossProcessMutex(CrossProcessMutexHandle aHandle);

  ~CrossProcessMutex();

  void Lock();

  void Unlock();

  CrossProcessMutexHandle CloneHandle();

 private:
  friend struct IPC::ParamTraits<CrossProcessMutex>;

  CrossProcessMutex();
  CrossProcessMutex(const CrossProcessMutex&);
  CrossProcessMutex& operator=(const CrossProcessMutex&);

  mozilla::ipc::SharedMemoryMappingWithHandle mSharedBuffer;
  pthread_mutex_t* mMutex;
  mozilla::Atomic<int32_t>* mCount;
};

typedef detail::BaseAutoLock<CrossProcessMutex&> CrossProcessMutexAutoLock;
typedef detail::BaseAutoUnlock<CrossProcessMutex&> CrossProcessMutexAutoUnlock;

}  

#endif
