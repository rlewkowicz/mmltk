/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsBaseAppShell_h_
#define nsBaseAppShell_h_

#include "mozilla/Atomics.h"
#include "nsIAppShell.h"
#include "nsIThreadInternal.h"
#include "nsIObserver.h"
#include "nsIRunnable.h"
#include "nsCOMPtr.h"
#include "nsTArray.h"
#include "prinrval.h"

class nsBaseAppShell : public nsIAppShell,
                       public nsIThreadObserver,
                       public nsIObserver {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIAPPSHELL

  NS_DECL_NSITHREADOBSERVER
  NS_DECL_NSIOBSERVER

  nsBaseAppShell();

  static void OnSystemTimezoneChange();

 protected:
  virtual ~nsBaseAppShell();

  nsresult Init();

  void NativeEventCallback();

  virtual void DoProcessMoreGeckoEvents();

  virtual void ScheduleNativeEventCallback() = 0;

  virtual bool ProcessNextNativeEvent(bool mayWait) = 0;

  int32_t mSuspendNativeCount;
  uint32_t mEventloopNestingLevel;

 private:
  bool DoProcessNextNativeEvent(bool mayWait);

  bool DispatchDummyEvent(nsIThread* target);

  void IncrementEventloopNestingLevel();
  void DecrementEventloopNestingLevel();

  nsCOMPtr<nsIRunnable> mDummyEvent;
  bool* mBlockedWait;
  mozilla::Atomic<bool> mNativeEventPending;
  PRIntervalTime mGeckoTaskBurstStartTime;
  PRIntervalTime mLastNativeEventTime;
  enum EventloopNestingState {
    eEventloopNone,   
    eEventloopXPCOM,  
    eEventloopOther   
  };
  EventloopNestingState mEventloopNestingState;
  bool mRunning;
  bool mExiting;
  bool mBlockNativeEvent;
  bool mProcessedGeckoEvents;
};

#endif  // nsBaseAppShell_h_
