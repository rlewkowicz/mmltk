/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_ScopeExit_h
#define mozilla_ScopeExit_h


#include <utility>

#include "mozilla/Attributes.h"

namespace mozilla {

template <typename ExitFunction>
class MOZ_STACK_CLASS ScopeExit {
  ExitFunction mExitFunction;
  bool mExecuteOnDestruction;

 public:
  explicit ScopeExit(ExitFunction&& cleanup)
      : mExitFunction(std::move(cleanup)), mExecuteOnDestruction(true) {}

  ScopeExit(ScopeExit&& rhs)
      : mExitFunction(std::move(rhs.mExitFunction)),
        mExecuteOnDestruction(rhs.mExecuteOnDestruction) {
    rhs.release();
  }

  ~ScopeExit() {
    if (mExecuteOnDestruction) {
      mExitFunction();
    }
  }

  void release() { mExecuteOnDestruction = false; }

 private:
  explicit ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;
  ScopeExit& operator=(ScopeExit&&) = delete;
};

template <typename ExitFunction>
[[nodiscard]] ScopeExit<ExitFunction> MakeScopeExit(
    ExitFunction&& exitFunction) {
  return ScopeExit<ExitFunction>(std::move(exitFunction));
}

} 

#endif /* mozilla_ScopeExit_h */
