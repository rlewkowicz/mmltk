/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsMemory.h"
#include "nsThreadUtils.h"

#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsIRunnable.h"
#include "nsISimpleEnumerator.h"

#include "nsCOMPtr.h"
#include "mozilla/Services.h"
#include "mozilla/Atomics.h"
#include "mozilla/IntegerPrintfMacros.h"


static mozilla::Atomic<bool> sIsFlushing;
static PRIntervalTime sLastFlushTime = 0;

bool nsMemory::IsLowMemoryPlatform() {
  return false;
}

static void RunFlushers(const char16_t* aReason) {
  nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService();
  if (os) {

    nsCOMPtr<nsISimpleEnumerator> e;
    os->EnumerateObservers("memory-pressure", getter_AddRefs(e));

    if (e) {
      nsCOMPtr<nsIObserver> observer;
      bool loop = true;

      while (NS_SUCCEEDED(e->HasMoreElements(&loop)) && loop) {
        nsCOMPtr<nsISupports> supports;
        e->GetNext(getter_AddRefs(supports));

        if (!supports) {
          continue;
        }

        observer = do_QueryInterface(supports);
        observer->Observe(observer, "memory-pressure", aReason);
      }
    }
  }

  sIsFlushing = false;
}

static nsresult FlushMemory(const char16_t* aReason, bool aImmediate) {
  if (aImmediate) {
    if (!NS_IsMainThread()) {
      NS_ERROR("can't synchronously flush memory: not on UI thread");
      return NS_ERROR_FAILURE;
    }
  }

  bool lastVal = sIsFlushing.exchange(true);
  if (lastVal) {
    return NS_OK;
  }

  PRIntervalTime now = PR_IntervalNow();

  nsresult rv = NS_OK;
  if (aImmediate) {
    RunFlushers(aReason);
  } else {
    if (PR_IntervalToMicroseconds(now - sLastFlushTime) > 1000) {
      nsCOMPtr<nsIRunnable> runnable(NS_NewRunnableFunction(
          "FlushMemory",
          [reason = aReason]() -> void { RunFlushers(reason); }));
      NS_DispatchToMainThread(runnable.forget());
    }
  }

  sLastFlushTime = now;
  return rv;
}

nsresult nsMemory::HeapMinimize(bool aImmediate) {
  return FlushMemory(u"heap-minimize", aImmediate);
}
