/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef gc_GCLock_h
#define gc_GCLock_h

#include "vm/Runtime.h"

namespace js {

class MOZ_RAII AutoLockGC {
 public:
  explicit AutoLockGC(gc::GCRuntime* gc) : gc(gc) { lock(); }
  explicit AutoLockGC(JSRuntime* rt) : AutoLockGC(&rt->gc) {}

  ~AutoLockGC() { lockGuard_.reset(); }

  LockGuard<Mutex>& guard() { return lockGuard_.ref(); }

 protected:
  void lock() {
    MOZ_ASSERT(lockGuard_.isNothing());
    lockGuard_.emplace(gc->lock);
  }

  void unlock() {
    MOZ_ASSERT(lockGuard_.isSome());
    lockGuard_.reset();
  }

  gc::GCRuntime* const gc;

 private:
  mozilla::Maybe<LockGuard<Mutex>> lockGuard_;

  AutoLockGC(const AutoLockGC&) = delete;
  AutoLockGC& operator=(const AutoLockGC&) = delete;

  friend class UnlockGuard<AutoLockGC>;  
};

class MOZ_RAII AutoLockGCBgAlloc : public AutoLockGC {
 public:
  explicit AutoLockGCBgAlloc(gc::GCRuntime* gc) : AutoLockGC(gc) {}
  explicit AutoLockGCBgAlloc(JSRuntime* rt) : AutoLockGCBgAlloc(&rt->gc) {}

  ~AutoLockGCBgAlloc() {
    unlock();

    if (startBgAlloc) {
      gc->startBackgroundAllocTaskIfIdle();
    }
  }

  void tryToStartBackgroundAllocation() { startBgAlloc = true; }

 private:
  bool startBgAlloc = false;
};

using AutoUnlockGC = UnlockGuard<AutoLockGC>;

}  

#endif /* gc_GCLock_h */
