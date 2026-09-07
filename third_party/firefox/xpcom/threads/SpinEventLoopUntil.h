/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef xpcom_threads_SpinEventLoopUntil_h_
#define xpcom_threads_SpinEventLoopUntil_h_

#include "MainThreadUtils.h"
#include "mozilla/Maybe.h"
#include "mozilla/StaticMutex.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "xpcpublic.h"

class nsIThread;

namespace mozilla {


enum class ProcessFailureBehavior {
  IgnoreAndContinue,
  ReportToCaller,
};

struct MOZ_STACK_CLASS AutoNestedEventLoopAnnotation {
  explicit AutoNestedEventLoopAnnotation(const nsACString& aEntry)
      : mPrev(nullptr) {
    if (NS_IsMainThread()) {
      StaticMutexAutoLock lock(sStackMutex);
      mPrev = sCurrent;
      sCurrent = this;
      if (mPrev) {
        mStack = mPrev->mStack + "|"_ns + aEntry;
      } else {
        mStack = aEntry;
      }
    }
  }

  ~AutoNestedEventLoopAnnotation() {
    if (NS_IsMainThread()) {
      StaticMutexAutoLock lock(sStackMutex);
      MOZ_ASSERT(sCurrent == this);
      sCurrent = mPrev;
    }
  }

  static void CopyCurrentStack(nsCString& aNestedSpinStack) {
    StaticMutexAutoLock lock(sStackMutex);
    if (sCurrent) {
      aNestedSpinStack = sCurrent->mStack;
    } else {
      aNestedSpinStack = "(no nested event loop active)"_ns;
    }
  }

 private:
  AutoNestedEventLoopAnnotation(const AutoNestedEventLoopAnnotation&) = delete;
  AutoNestedEventLoopAnnotation& operator=(
      const AutoNestedEventLoopAnnotation&) = delete;

  static AutoNestedEventLoopAnnotation* sCurrent MOZ_GUARDED_BY(sStackMutex);
  static StaticMutex sStackMutex;

  AutoNestedEventLoopAnnotation* mPrev MOZ_GUARDED_BY(sStackMutex);
  nsCString mStack MOZ_GUARDED_BY(sStackMutex);
};

template <
    ProcessFailureBehavior Behavior = ProcessFailureBehavior::ReportToCaller,
    typename Pred>
bool SpinEventLoopUntil(const nsACString& aVeryGoodReasonToDoThis,
                        Pred&& aPredicate, nsIThread* aThread = nullptr) {
  AutoNestedEventLoopAnnotation annotation(aVeryGoodReasonToDoThis);

  nsIThread* thread = aThread ? aThread : NS_GetCurrentThread();

  mozilla::Maybe<xpc::AutoScriptActivity> asa;
  if (NS_IsMainThread()) {
    asa.emplace(false);
  }

  while (!aPredicate()) {
    bool didSomething = NS_ProcessNextEvent(thread, true);

    if (Behavior == ProcessFailureBehavior::IgnoreAndContinue) {
      continue;
    } else if (!didSomething) {
      return false;
    }
  }

  return true;
}

}  

#endif  // xpcom_threads_SpinEventLoopUntil_h_
