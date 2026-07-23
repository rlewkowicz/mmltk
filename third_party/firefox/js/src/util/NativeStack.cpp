/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "util/NativeStack.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_RELEASE_ASSERT, MOZ_CRASH

#if defined(__wasi__)
#elif 0 || 0 || defined(XP_UNIX)
#  include <pthread.h>
#if defined(XP_LINUX) && !0 && defined(__GLIBC__)
#    include <dlfcn.h>
#    include <sys/syscall.h>
#    include <sys/types.h>
#    include <unistd.h>
#    define gettid() static_cast<pid_t>(syscall(__NR_gettid))
#endif
#else
#  error "Unsupported platform"
#endif

#include "js/friend/StackLimits.h"  // JS_STACK_GROWTH_DIRECTION

#if defined(XP_LINUX) && !0 && defined(__GLIBC__)
void* js::GetNativeStackBaseImpl() {
  if (gettid() == getpid()) {
    void** pLibcStackEnd = (void**)dlsym(RTLD_DEFAULT, "__libc_stack_end");

    MOZ_RELEASE_ASSERT(
        pLibcStackEnd,
        "__libc_stack_end unavailable, unable to setup stack range for JS");
    void* stackBase = *pLibcStackEnd;
    MOZ_RELEASE_ASSERT(
        stackBase, "invalid stack base, unable to setup stack range for JS");

    return stackBase;
  }

  pthread_t thread = pthread_self();
  pthread_attr_t sattr;
  pthread_attr_init(&sattr);
  int rc = pthread_getattr_np(thread, &sattr);
  MOZ_RELEASE_ASSERT(rc == 0, "pthread_getattr_np failed");

  void* stackBase = nullptr;
  size_t stackSize = 0;
  rc = pthread_attr_getstack(&sattr, &stackBase, &stackSize);
  MOZ_RELEASE_ASSERT(rc == 0,
                     "call to pthread_attr_getstack failed, unable to setup "
                     "stack range for JS");
  MOZ_RELEASE_ASSERT(stackBase,
                     "invalid stack base, unable to setup stack range for JS");
  pthread_attr_destroy(&sattr);

#if JS_STACK_GROWTH_DIRECTION > 0
  return stackBase;
#else
  return static_cast<char*>(stackBase) + stackSize;
#endif
}

#elif defined(__wasi__)

static void* const NativeStackBase = __builtin_frame_address(0);

void* js::GetNativeStackBaseImpl() {
  MOZ_ASSERT(JS_STACK_GROWTH_DIRECTION < 0);
  return NativeStackBase;
}

#else

void* js::GetNativeStackBaseImpl() {
  pthread_t thread = pthread_self();
  pthread_attr_t sattr;
  pthread_attr_init(&sattr);
#if defined(PTHREAD_NP_H) || defined(_PTHREAD_NP_H_) || defined(NETBSD)
  pthread_attr_get_np(thread, &sattr);
#else
  MOZ_RELEASE_ASSERT(pthread_getattr_np(thread, &sattr) == 0,
                     "pthread_getattr_np failed");
#endif

  void* stackBase = 0;
  size_t stackSize = 0;
  int rc;
  rc = pthread_attr_getstack(&sattr, &stackBase, &stackSize);
  if (rc) {
    MOZ_CRASH();
  }
  MOZ_ASSERT(stackBase);
  pthread_attr_destroy(&sattr);

#if JS_STACK_GROWTH_DIRECTION > 0
  return stackBase;
#else
  return static_cast<char*>(stackBase) + stackSize;
#endif
}

#endif
