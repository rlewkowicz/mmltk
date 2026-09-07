/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(threading_posix_PlatformData_h)
#define threading_posix_PlatformData_h

#include <pthread.h>



#if defined(__linux__)
#  include <sys/prctl.h>
#endif

#include "threading/Thread.h"

namespace js {

class ThreadId::PlatformData {
  friend class Thread;
  friend class ThreadId;
  pthread_t ptThread;

  bool hasThread;
};

}  

#endif
