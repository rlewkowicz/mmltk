/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "mozilla/ShutdownPhase.h"
#include "nsTerminator.h"

#include "prthread.h"
#include "prmon.h"
#include "prio.h"

#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"

#  include <unistd.h>

#include "mozilla/AppShutdown.h"
#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/IntentionalCrash.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/Preferences.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "mozilla/UniquePtr.h"

#include "mozilla/dom/workerinternals/RuntimeService.h"

#define FALLBACK_ASYNCSHUTDOWN_CRASH_AFTER_MS 60000

#define ADDITIONAL_WAIT_BEFORE_CRASH_MS 3000

#define HEARTBEAT_INTERVAL_MS 100

namespace mozilla {

namespace {

struct ShutdownStep {
  mozilla::ShutdownPhase mPhase;
  Atomic<int> mTicks;

  constexpr explicit ShutdownStep(mozilla::ShutdownPhase aPhase)
      : mPhase(aPhase), mTicks(-1) {}
};

static ShutdownStep sShutdownSteps[] = {
    ShutdownStep(mozilla::ShutdownPhase::AppShutdownConfirmed),
    ShutdownStep(mozilla::ShutdownPhase::AppShutdownNetTeardown),
    ShutdownStep(mozilla::ShutdownPhase::AppShutdownTeardown),
    ShutdownStep(mozilla::ShutdownPhase::AppShutdown),
    ShutdownStep(mozilla::ShutdownPhase::AppShutdownQM),
    ShutdownStep(mozilla::ShutdownPhase::XPCOMWillShutdown),
    ShutdownStep(mozilla::ShutdownPhase::XPCOMShutdown),
    ShutdownStep(mozilla::ShutdownPhase::XPCOMShutdownThreads),
    ShutdownStep(mozilla::ShutdownPhase::XPCOMShutdownFinal),
    ShutdownStep(mozilla::ShutdownPhase::CCPostLastCycleCollection),
};

int GetStepForPhase(mozilla::ShutdownPhase aPhase) {
  for (size_t i = 0; i < std::size(sShutdownSteps); i++) {
    if (sShutdownSteps[i].mPhase >= aPhase) {
      return (int)i;
    }
  }
  return -1;
}

PRThread* CreateSystemThread(void (*start)(void* arg), void* arg) {
  PRThread* thread =
      PR_CreateThread(PR_SYSTEM_THREAD, 
                      start, arg, PR_PRIORITY_LOW,
                      PR_GLOBAL_THREAD, 
                      PR_UNJOINABLE_THREAD, 0 
      );
  MOZ_LSAN_INTENTIONALLY_LEAK_OBJECT(
      thread);  
  return thread;
}


Atomic<uint32_t> gHeartbeat(0);

struct Options {
  uint32_t crashAfterTicks;
};

void RunWatchdog(void* arg) {
  NS_SetCurrentThreadName("Shutdown Hang Terminator");

  UniquePtr<Options> options((Options*)arg);
  uint32_t crashAfterTicks = options->crashAfterTicks;
  options = nullptr;

  const uint32_t timeToLive = crashAfterTicks;
  while (true) {
    usleep(HEARTBEAT_INTERVAL_MS * 1000 );

    if (gHeartbeat++ < timeToLive) {
      continue;
    }


    NoteIntentionalCrash(XRE_GetProcessTypeString());

    nsCString stack;
    AutoNestedEventLoopAnnotation::CopyCurrentStack(stack);
    printf_stderr(
        "RunWatchdog: Mainthread nested event loops during hang: \n --- %s\n",
        stack.get());

    mozilla::ShutdownPhase lastPhase = mozilla::ShutdownPhase::NotInShutdown;
    for (int i = std::size(sShutdownSteps) - 1; i >= 0; --i) {
      if (sShutdownSteps[i].mTicks > -1) {
        lastPhase = sShutdownSteps[i].mPhase;
        break;
      }
    }

    if (lastPhase == mozilla::ShutdownPhase::NotInShutdown) {
      MOZ_CRASH("Shutdown hanging before starting any known phase.");
    }

    mozilla::dom::workerinternals::RuntimeService* runtimeService =
        mozilla::dom::workerinternals::RuntimeService::GetService();
    if (runtimeService) {
      runtimeService->CrashIfHanging();
    }

    nsCString msg;
    msg.AppendPrintf(
        "Shutdown hanging at step %s. "
        "Something is blocking the main-thread.",
        mozilla::AppShutdown::GetShutdownPhaseName(lastPhase));

    MOZ_CRASH_UNSAFE(strdup(msg.get()));
  }
}

}  

NS_IMPL_ISUPPORTS(nsTerminator, nsISupports)

nsTerminator::nsTerminator() : mInitialized(false), mCurrentStep(-1) {}

void nsTerminator::Start() {
  MOZ_ASSERT(!mInitialized);

  StartWatchdog();
  mInitialized = true;
}

void nsTerminator::StartWatchdog() {
  int32_t crashAfterMS =
      Preferences::GetInt("toolkit.asyncshutdown.crash_timeout",
                          FALLBACK_ASYNCSHUTDOWN_CRASH_AFTER_MS);

  int32_t additionalWaitBeforeCrashMs =
      Preferences::GetInt("toolkit.asyncshutdown.crash_timeout_additional_wait",
                          ADDITIONAL_WAIT_BEFORE_CRASH_MS);

  if (crashAfterMS <= 0) {
    crashAfterMS = FALLBACK_ASYNCSHUTDOWN_CRASH_AFTER_MS;
  }

  if (additionalWaitBeforeCrashMs <= 0) {
    additionalWaitBeforeCrashMs = ADDITIONAL_WAIT_BEFORE_CRASH_MS;
  }

  if (crashAfterMS > INT32_MAX - additionalWaitBeforeCrashMs) {
    crashAfterMS = INT32_MAX;
  } else {
    crashAfterMS += additionalWaitBeforeCrashMs;
  }

#if defined(MOZ_VALGRIND)
  if (RUNNING_ON_VALGRIND) {
    const int32_t scaleUp = 3;
    if (crashAfterMS >= (INT32_MAX / scaleUp) - 1) {
      crashAfterMS = INT32_MAX;
    } else {
      crashAfterMS *= scaleUp;
    }
  }
#endif

  UniquePtr<Options> options(new Options());
  options->crashAfterTicks = std::max(1, crashAfterMS / HEARTBEAT_INTERVAL_MS);

  DebugOnly<PRThread*> watchdogThread =
      CreateSystemThread(RunWatchdog, options.release());
  MOZ_ASSERT(watchdogThread);
}

const char* GetReadableNameForPhase(mozilla::ShutdownPhase aPhase) {
  const char* readableName = mozilla::AppShutdown::GetObserverKey(aPhase);
  if (!readableName) {
    readableName = mozilla::AppShutdown::GetShutdownPhaseName(aPhase);
  }
  return readableName;
}

void nsTerminator::AdvancePhase(mozilla::ShutdownPhase aPhase) {
  auto step = GetStepForPhase(aPhase);
  if (step < 0) {
    return;
  }

  if (!mInitialized) {
    Start();
  }

  UpdateHeartbeat(step);
}

void nsTerminator::UpdateHeartbeat(int32_t aStep) {
  MOZ_ASSERT(aStep >= mCurrentStep);

  if (aStep > mCurrentStep) {
    uint32_t ticks = gHeartbeat.exchange(0);
    if (mCurrentStep >= 0) {
      sShutdownSteps[mCurrentStep].mTicks = ticks;
    }
    sShutdownSteps[aStep].mTicks = 0;

    mCurrentStep = aStep;
  }
}

}  
