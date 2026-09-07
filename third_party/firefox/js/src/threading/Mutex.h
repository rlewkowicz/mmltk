/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_Mutex_h
#define threading_Mutex_h

#include "mozilla/Assertions.h"
#include "mozilla/PlatformMutex.h"
#include "mozilla/ThreadLocal.h"

#include "threading/ThreadId.h"

namespace js {

struct MutexId {
  const char* name;
  uint32_t order;
};

class MutexImpl : public mozilla::detail::MutexImpl {
 protected:
  MutexImpl() = default;

  friend class Mutex;
};

class Mutex {
 private:
  MutexImpl impl_;

#ifdef DEBUG
  const MutexId id_;
  Mutex* prev_ = nullptr;
  ThreadId owningThread_;

  static MOZ_THREAD_LOCAL(Mutex*) HeldMutexStack;
#endif

 public:
#ifdef DEBUG
  static bool Init();

  explicit Mutex(const MutexId& id) : id_(id) { MOZ_ASSERT(id_.order != 0); }

  void lock();
  bool tryLock();
  void unlock();
  bool isOwnedByCurrentThread() const;
  void assertOwnedByCurrentThread() const;
#else
  static bool Init() { return true; }

  explicit Mutex(const MutexId& id) {}

  void lock() { impl_.lock(); }
  bool tryLock() { return impl_.tryLock(); }
  void unlock() { impl_.unlock(); }
  void assertOwnedByCurrentThread() const {};
#endif

 private:
#ifdef DEBUG
  void preLockChecks() const;
  void postLockChecks();
  void preUnlockChecks();
#endif

  friend class ConditionVariable;
};

}  

#endif  // threading_Mutex_h
