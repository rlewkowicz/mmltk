/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedScriptDataTableHolder_h
#define vm_SharedScriptDataTableHolder_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include "threading/Mutex.h"   // js::Mutex
#include "vm/SharedStencil.h"  // js::SharedImmutableScriptDataTable

namespace js {

class AutoLockGlobalScriptData {
  static js::Mutex mutex_;

 public:
  AutoLockGlobalScriptData();
  ~AutoLockGlobalScriptData();
};

class SharedScriptDataTableHolder {
  bool needsLock_ = true;
  js::SharedImmutableScriptDataTable scriptDataTable_;

 public:
  enum class NeedsLock { No, Yes };

  explicit SharedScriptDataTableHolder(NeedsLock needsLock = NeedsLock::Yes)
      : needsLock_(needsLock == NeedsLock::Yes) {}

  js::SharedImmutableScriptDataTable& get(
      const js::AutoLockGlobalScriptData& lock) {
    MOZ_ASSERT(needsLock_);
    return scriptDataTable_;
  }

  js::SharedImmutableScriptDataTable& getWithoutLock() {
    MOZ_ASSERT(!needsLock_);
    return scriptDataTable_;
  }

  js::SharedImmutableScriptDataTable& getMaybeLocked(
      mozilla::Maybe<js::AutoLockGlobalScriptData>& lock) {
    if (needsLock_) {
      lock.emplace();
    }
    return scriptDataTable_;
  }
};

extern SharedScriptDataTableHolder globalSharedScriptDataTableHolder;

} 

#endif /* vm_SharedScriptDataTableHolder_h */
