// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/platform_thread.h"

#include <errno.h>
#include <sched.h>

#if defined(XP_LINUX)
#  include <sys/syscall.h>
#  include <sys/prctl.h>
#endif

#  include <unistd.h>


#include "nsThreadUtils.h"


static void* ThreadFunc(void* closure) {
  PlatformThread::Delegate* delegate =
      static_cast<PlatformThread::Delegate*>(closure);
  delegate->ThreadMain();
  return nullptr;
}

PlatformThreadId PlatformThread::CurrentId() {
#if defined(XP_LINUX)
  return syscall(__NR_gettid);
#elif 0 || 0 || defined(__GLIBC__)
  return (intptr_t)(pthread_self());
#endif
}

void PlatformThread::YieldCurrentThread() { sched_yield(); }

void PlatformThread::Sleep(int duration_ms) {
  struct timespec sleep_time, remaining;

  sleep_time.tv_sec = duration_ms / 1000;
  duration_ms -= sleep_time.tv_sec * 1000;

  sleep_time.tv_nsec = duration_ms * 1000 * 1000;  

  while (nanosleep(&sleep_time, &remaining) == -1 && errno == EINTR)
    sleep_time = remaining;
}


void PlatformThread::SetName(const char* name) {
  if (PlatformThread::CurrentId() == getpid()) return;

  NS_SetCurrentThreadName(name);
}

namespace {

bool CreateThread(size_t stack_size, bool joinable,
                  PlatformThread::Delegate* delegate,
                  PlatformThreadHandle* thread_handle) {

  bool success = false;
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);

  if (!joinable) {
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  }

  if (stack_size == 0) stack_size = nsIThreadManager::DEFAULT_STACK_SIZE;
  pthread_attr_setstacksize(&attributes, stack_size);

  success = !pthread_create(thread_handle, &attributes, ThreadFunc, delegate);

  pthread_attr_destroy(&attributes);
  return success;
}

}  

bool PlatformThread::Create(size_t stack_size, Delegate* delegate,
                            PlatformThreadHandle* thread_handle) {
  return CreateThread(stack_size, true , delegate,
                      thread_handle);
}

bool PlatformThread::CreateNonJoinable(size_t stack_size, Delegate* delegate) {
  PlatformThreadHandle unused;

  bool result = CreateThread(stack_size, false ,
                             delegate, &unused);
  return result;
}

void PlatformThread::Join(PlatformThreadHandle thread_handle) {
  pthread_join(thread_handle, nullptr);
}
