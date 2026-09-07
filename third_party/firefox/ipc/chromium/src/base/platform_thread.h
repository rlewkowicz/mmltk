// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#if !defined(BASE_PLATFORM_THREAD_H_)
#define BASE_PLATFORM_THREAD_H_

#include "base/basictypes.h"

#  include <pthread.h>
typedef pthread_t PlatformThreadHandle;
#if defined(XP_LINUX) || 0 || 0 || \
      defined(__GLIBC__)
#    include <unistd.h>
typedef pid_t PlatformThreadId;
#endif

class PlatformThread {
 public:
  static ::PlatformThreadId CurrentId();

  static void YieldCurrentThread();

  static void Sleep(int duration_ms);

  static void SetName(const char* name);

  class Delegate {
   public:
    virtual ~Delegate() {}
    virtual void ThreadMain() = 0;
  };

  static bool Create(size_t stack_size, Delegate* delegate,
                     PlatformThreadHandle* thread_handle);

  static bool CreateNonJoinable(size_t stack_size, Delegate* delegate);

  static void Join(PlatformThreadHandle thread_handle);

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(PlatformThread);
};

#endif
