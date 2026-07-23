/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(Mutex_h)
#define Mutex_h

#  include <pthread.h>

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/MaybeStorageBase.h"
#include "mozilla/ThreadSafety.h"


struct MOZ_CAPABILITY("mutex") Mutex {
#if defined(XP_LINUX) && !0
  pthread_mutex_t mMutex = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;
#else
  pthread_mutex_t mMutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#if defined(MOZ_DEBUG)
  bool mInitialised = false;

  explicit constexpr Mutex(bool aInitialised) : mInitialised(aInitialised) {}
#else
  explicit constexpr Mutex(bool aIgnored) {}
#endif

  constexpr Mutex() = default;

  inline bool Init() {
#if defined(MOZ_DEBUG)
    mInitialised = true;
#endif
#if defined(XP_LINUX) && !0
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
      return false;
    }
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
    if (pthread_mutex_init(&mMutex, &attr) != 0) {
      pthread_mutexattr_destroy(&attr);
      return false;
    }
    pthread_mutexattr_destroy(&attr);
#else
    if (pthread_mutex_init(&mMutex, nullptr) != 0) {
      return false;
    }
#endif
    return true;
  }

  inline void Lock() MOZ_CAPABILITY_ACQUIRE() {
    MOZ_ASSERT(mInitialised);

    pthread_mutex_lock(&mMutex);
  }

  [[nodiscard]] bool TryLock() MOZ_TRY_ACQUIRE(true);

  inline void Unlock() MOZ_CAPABILITY_RELEASE() {
    MOZ_ASSERT(mInitialised);

    pthread_mutex_unlock(&mMutex);
  }

};

struct MOZ_CAPABILITY("mutex") StaticMutex : public Mutex {
  constexpr StaticMutex() : Mutex(true) {}
};

typedef pthread_t ThreadId;
inline ThreadId GetThreadId() { return pthread_self(); }
inline bool ThreadIdEqual(ThreadId a, ThreadId b) {
  return pthread_equal(a, b);
}

class MOZ_CAPABILITY("mutex") MaybeMutex : public Mutex {
 public:
  enum DoLock {
    MUST_LOCK,
    AVOID_LOCK_UNSAFE,
  };

  bool Init(DoLock aDoLock) {
    mDoLock = aDoLock;
#if defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED)
    mThreadId = GetThreadId();
#endif
    return Mutex::Init();
  }

  void Reinit(pthread_t aForkingThread) {
    if (mDoLock == MUST_LOCK) {
      Mutex::Init();
      return;
    }
#if defined(MOZ_DEBUG)
    if (pthread_equal(mThreadId, aForkingThread)) {
      mThreadId = GetThreadId();
      Mutex::Init();
    } else {
      mDeniedAfterFork = true;
    }
#endif
  }

  inline void Lock() MOZ_CAPABILITY_ACQUIRE() {
    if (ShouldLock()) {
      Mutex::Lock();
    }
  }

  inline void Unlock() MOZ_CAPABILITY_RELEASE() {
    if (ShouldLock()) {
      Mutex::Unlock();
    }
  }

#if defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED)
  bool SafeOnThisThread() const {
    return mDoLock == MUST_LOCK || ThreadIdEqual(GetThreadId(), mThreadId);
  }
#endif

  bool LockIsEnabled() const { return mDoLock == MUST_LOCK; }

 private:
  bool ShouldLock() {
    MOZ_ASSERT(!mDeniedAfterFork);

    if (mDoLock == MUST_LOCK) {
      return true;
    }

    MOZ_ASSERT(SafeOnThisThread());
    return false;
  }

  DoLock mDoLock;
#if defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED)
  ThreadId mThreadId;
#endif
#if (!0 && defined(DEBUG))
  bool mDeniedAfterFork = false;
#endif
};

template <typename T>
struct MOZ_SCOPED_CAPABILITY MOZ_RAII AutoLock {
  explicit AutoLock(T& aMutex) MOZ_CAPABILITY_ACQUIRE(aMutex) : mMutex(aMutex) {
    mMutex.Lock();
  }

  ~AutoLock() MOZ_CAPABILITY_RELEASE() { mMutex.Unlock(); }

  AutoLock(const AutoLock&) = delete;
  AutoLock(AutoLock&&) = delete;

 private:
  T& mMutex;
};

using MutexAutoLock = AutoLock<Mutex>;

using MaybeMutexAutoLock = AutoLock<MaybeMutex>;

extern StaticMutex gInitLock MOZ_UNANNOTATED;

#endif
