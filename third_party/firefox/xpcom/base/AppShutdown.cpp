/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ShutdownPhase.h"
#include "mozilla/ScopeExit.h"
#  include <unistd.h>
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/CmdLineAndEnvUtils.h"
#include "mozilla/PoisonIOInterposer.h"
#include "mozilla/Printf.h"
#include "mozilla/scache/StartupCache.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "mozilla/StaticPrefs_toolkit.h"
#include "mozilla/Services.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsAppRunner.h"
#include "nsDirectoryServiceUtils.h"
#include "nsICertStorage.h"
#include "nsThreadUtils.h"

#include "AppShutdown.h"

#  include "nsTerminator.h"
#include "prenv.h"

#if defined(MOZ_BACKGROUNDTASKS)
#  include "mozilla/BackgroundTasks.h"
#endif

namespace mozilla {

const char* sPhaseObserverKeys[] = {
    nullptr,                            
    "quit-application",                 
    "profile-change-net-teardown",      
    "profile-change-teardown",          
    "profile-before-change",            
    "profile-before-change-qm",         
    "profile-before-change-telemetry",  
    "xpcom-will-shutdown",              
    "xpcom-shutdown",                   
    "xpcom-shutdown-threads",           
    nullptr,                            
    nullptr                             
};

static_assert(sizeof(sPhaseObserverKeys) / sizeof(sPhaseObserverKeys[0]) ==
              (size_t)ShutdownPhase::ShutdownPhase_Length);

const char* sPhaseReadableNames[] = {"NotInShutdown",
                                     "AppShutdownConfirmed",
                                     "AppShutdownNetTeardown",
                                     "AppShutdownTeardown",
                                     "AppShutdown",
                                     "AppShutdownQM",
                                     "AppShutdownTelemetry",
                                     "XPCOMWillShutdown",
                                     "XPCOMShutdown",
                                     "XPCOMShutdownThreads",
                                     "XPCOMShutdownFinal",
                                     "CCPostLastCycleCollection"};

static_assert(sizeof(sPhaseReadableNames) / sizeof(sPhaseReadableNames[0]) ==
              (size_t)ShutdownPhase::ShutdownPhase_Length);

static nsTerminator* sTerminator = nullptr;

static ShutdownPhase sFastShutdownPhase = ShutdownPhase::NotInShutdown;
static AppShutdownMode sShutdownMode = AppShutdownMode::Normal;
static Atomic<ShutdownPhase> sCurrentShutdownPhase(
    ShutdownPhase::NotInShutdown);
static int sExitCode = 0;
static Atomic<bool> sShutdownImpending(false);

static char* sSavedXulAppFile = nullptr;
static char* sSavedProfDEnvVar = nullptr;
static char* sSavedProfLDEnvVar = nullptr;

bool AppShutdown::IsShutdownImpending() { return sShutdownImpending; }

void AppShutdown::SetImpendingShutdown() { sShutdownImpending = true; }

ShutdownPhase GetShutdownPhaseFromPrefValue(int32_t aPrefValue) {
  switch (aPrefValue) {
    case 1:
      return ShutdownPhase::CCPostLastCycleCollection;
    case 2:
      return ShutdownPhase::XPCOMShutdownThreads;
    case 3:
      return ShutdownPhase::XPCOMShutdown;
  }
  return ShutdownPhase::NotInShutdown;
}

ShutdownPhase AppShutdown::GetCurrentShutdownPhase() {
  return sCurrentShutdownPhase;
}

bool AppShutdown::IsInOrBeyond(ShutdownPhase aPhase) {
  return (sCurrentShutdownPhase >= aPhase);
}

int AppShutdown::GetExitCode() { return sExitCode; }

void AppShutdown::SaveEnvVarsForPotentialRestart() {
  const char* s = PR_GetEnv("XUL_APP_FILE");
  if (s) {
    sSavedXulAppFile = Smprintf("%s=%s", "XUL_APP_FILE", s).release();
    MOZ_LSAN_INTENTIONALLY_LEAK_OBJECT(sSavedXulAppFile);
  }
}

const char* AppShutdown::GetObserverKey(ShutdownPhase aPhase) {
  return sPhaseObserverKeys[static_cast<std::underlying_type_t<ShutdownPhase>>(
      aPhase)];
}

const char* AppShutdown::GetShutdownPhaseName(ShutdownPhase aPhase) {
  return sPhaseReadableNames[static_cast<std::underlying_type_t<ShutdownPhase>>(
      aPhase)];
}

void AppShutdown::MaybeDoRestart() {
  if (sShutdownMode == AppShutdownMode::Restart) {
    UnlockProfile();

    if (sSavedXulAppFile) {
      PR_SetEnv(sSavedXulAppFile);
    }

    if (sSavedProfDEnvVar && !EnvHasValue("XRE_PROFILE_PATH")) {
      PR_SetEnv(sSavedProfDEnvVar);
    }
    if (sSavedProfLDEnvVar && !EnvHasValue("XRE_PROFILE_LOCAL_PATH")) {
      PR_SetEnv(sSavedProfLDEnvVar);
    }

    LaunchChild(true);
  }
}


void AppShutdown::Init(AppShutdownMode aMode, int aExitCode,
                       AppShutdownReason aReason) {
  if (sShutdownMode == AppShutdownMode::Normal) {
    sShutdownMode = aMode;
  }
  AppShutdown::SetImpendingShutdown();

  sExitCode = aExitCode;

  sTerminator = new nsTerminator();

  int32_t fastShutdownPref = StaticPrefs::toolkit_shutdown_fastShutdownStage();
  sFastShutdownPhase = GetShutdownPhaseFromPrefValue(fastShutdownPref);

  if (auto* cache = scache::StartupCache::GetSingletonNoInit()) {
    cache->MaybeKickOffShutdownWrite();
  }
}

void AppShutdown::MaybeFastShutdown(ShutdownPhase aPhase) {
  if (aPhase == sFastShutdownPhase) {
    if (auto* cache = scache::StartupCache::GetSingletonNoInit()) {
      cache->EnsureShutdownWriteComplete();
    }

    nsresult rv;
    nsCOMPtr<nsICertStorage> certStorage =
        do_GetService("@mozilla.org/security/certstorage;1", &rv);
    if (NS_SUCCEEDED(rv)) {
      SpinEventLoopUntil("AppShutdown::MaybeFastShutdown"_ns, [&]() {
        int32_t remainingOps;
        nsresult rv = certStorage->GetRemainingOperationCount(&remainingOps);
        NS_ASSERTION(NS_SUCCEEDED(rv),
                     "nsICertStorage::getRemainingOperationCount failed during "
                     "shutdown");
        return NS_FAILED(rv) || remainingOps <= 0;
      });
    }
    MaybeDoRestart();

    DoImmediateExit(sExitCode);
  }
}

void AppShutdown::OnShutdownConfirmed() {
  if (sShutdownMode == AppShutdownMode::Restart) {
    nsCOMPtr<nsIFile> profD;
    nsCOMPtr<nsIFile> profLD;
    NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR, getter_AddRefs(profD));
    NS_GetSpecialDirectory(NS_APP_USER_PROFILE_LOCAL_50_DIR,
                           getter_AddRefs(profLD));
    nsAutoCString profDStr;
    profD->GetNativePath(profDStr);
    sSavedProfDEnvVar =
        Smprintf("XRE_PROFILE_PATH=%s", profDStr.get()).release();
    nsAutoCString profLDStr;
    profLD->GetNativePath(profLDStr);
    sSavedProfLDEnvVar =
        Smprintf("XRE_PROFILE_LOCAL_PATH=%s", profLDStr.get()).release();
    MOZ_LSAN_INTENTIONALLY_LEAK_OBJECT(sSavedProfDEnvVar);
    MOZ_LSAN_INTENTIONALLY_LEAK_OBJECT(sSavedProfLDEnvVar);
  }
}

void AppShutdown::DoImmediateExit(int aExitCode) {
  _exit(aExitCode);
}

bool AppShutdown::IsRestarting() {
  return sShutdownMode == AppShutdownMode::Restart;
}

#if defined(DEBUG)
static bool sNotifyingShutdownObservers = false;
static bool sAdvancingShutdownPhase = false;

bool AppShutdown::IsNoOrLegalShutdownTopic(const char* aTopic) {
  if (!XRE_IsParentProcess()) {
    return true;
  }
  ShutdownPhase phase = GetShutdownPhaseFromTopic(aTopic);
  return phase == ShutdownPhase::NotInShutdown ||
         (sNotifyingShutdownObservers && phase == sCurrentShutdownPhase);
}
#endif

void AppShutdown::AdvanceShutdownPhaseInternal(
    ShutdownPhase aPhase, bool doNotify, const char16_t* aNotificationData,
    const nsCOMPtr<nsISupports>& aNotificationSubject) {
  AssertIsOnMainThread();
#if defined(DEBUG)
  MOZ_ASSERT(!sAdvancingShutdownPhase);
  sAdvancingShutdownPhase = true;
  auto exit = MakeScopeExit([] { sAdvancingShutdownPhase = false; });
#endif

  if (sCurrentShutdownPhase >= aPhase) {
    return;
  }

  SetImpendingShutdown();

  nsCOMPtr<nsIThread> thread = do_GetCurrentThread();

  bool mayProcessPending = (aPhase > ShutdownPhase::AppShutdownConfirmed);

  if (mayProcessPending && thread) {
    NS_ProcessPendingEvents(thread);
  }

  sCurrentShutdownPhase = aPhase;

  if (sTerminator) {
    sTerminator->AdvancePhase(aPhase);
  }

  AppShutdown::MaybeFastShutdown(aPhase);

  mozilla::KillClearOnShutdown(aPhase);

  if (mayProcessPending && thread) {
    NS_ProcessPendingEvents(thread);
  }

  if (doNotify) {
    const char* aTopic = AppShutdown::GetObserverKey(aPhase);
    if (aTopic) {
      nsCOMPtr<nsIObserverService> obsService =
          mozilla::services::GetObserverService();
      if (obsService) {
#if defined(DEBUG)
        sNotifyingShutdownObservers = true;
        auto reset = MakeScopeExit([] { sNotifyingShutdownObservers = false; });
#endif
        obsService->NotifyObservers(aNotificationSubject, aTopic,
                                    aNotificationData);
        if (mayProcessPending && thread) {
          NS_ProcessPendingEvents(thread);
        }
      }
    }
  }
}

void AppShutdown::AdvanceShutdownPhaseWithoutNotify(ShutdownPhase aPhase) {
  AdvanceShutdownPhaseInternal(aPhase,  false, nullptr, nullptr);
}

void AppShutdown::AdvanceShutdownPhase(
    ShutdownPhase aPhase, const char16_t* aNotificationData,
    const nsCOMPtr<nsISupports>& aNotificationSubject) {
  AdvanceShutdownPhaseInternal(aPhase,  true, aNotificationData,
                               aNotificationSubject);
}

ShutdownPhase AppShutdown::GetShutdownPhaseFromTopic(const char* aTopic) {
  for (size_t i = 0; i < std::size(sPhaseObserverKeys); ++i) {
    if (sPhaseObserverKeys[i] && !strcmp(sPhaseObserverKeys[i], aTopic)) {
      return static_cast<ShutdownPhase>(i);
    }
  }
  return ShutdownPhase::NotInShutdown;
}

}  
