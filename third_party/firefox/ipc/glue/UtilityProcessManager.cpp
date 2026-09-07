/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "UtilityProcessManager.h"

#include "JSOracleParent.h"
#include "mozilla/ipc/UtilityProcessHost.h"
#include "mozilla/MemoryReportingProcess.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/SyncRunnable.h"  // for LaunchUtilityProcess
#include "mozilla/ipc/UtilityProcessParent.h"
#include "mozilla/ipc/UtilityMediaServiceChild.h"
#include "mozilla/ipc/UtilityMediaServiceParent.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/UtilityProcessTypes.h"
#include "mozilla/ipc/ProcessChild.h"
#include "nsAppRunner.h"
#include "nsContentUtils.h"


#include "mozilla/GeckoArgs.h"

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
#  include "mozilla/psm/PPKCS11ModuleChild.h"
#endif

namespace mozilla::ipc {

extern LazyLogModule gUtilityProcessLog;
#define LOGD(...) MOZ_LOG(gUtilityProcessLog, LogLevel::Debug, (__VA_ARGS__))

static StaticRefPtr<UtilityProcessManager> sSingleton;

static bool sXPCOMShutdown = false;

bool UtilityProcessManager::IsShutdown() const {
  MOZ_ASSERT(NS_IsMainThread());
  return sXPCOMShutdown || !sSingleton;
}

RefPtr<UtilityProcessManager> UtilityProcessManager::GetSingleton() {
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(NS_IsMainThread());

  if (!sXPCOMShutdown && sSingleton == nullptr) {
    sSingleton = new UtilityProcessManager();
    sSingleton->Init();
  }
  return sSingleton;
}

RefPtr<UtilityProcessManager> UtilityProcessManager::GetIfExists() {
  MOZ_ASSERT(NS_IsMainThread());
  return sSingleton;
}

UtilityProcessManager::UtilityProcessManager() {
  LOGD("[%p] UtilityProcessManager::UtilityProcessManager", this);
}

void UtilityProcessManager::Init() {
  mObserver = new Observer(this);
  nsContentUtils::RegisterShutdownObserver(mObserver);
  Preferences::AddStrongObserver(mObserver, "");
}

UtilityProcessManager::~UtilityProcessManager() {
  LOGD("[%p] UtilityProcessManager::~UtilityProcessManager", this);

  MOZ_ASSERT(NoMoreProcesses());
}

NS_IMPL_ISUPPORTS(UtilityProcessManager::Observer, nsIObserver);

UtilityProcessManager::Observer::Observer(UtilityProcessManager* aManager)
    : mManager(aManager) {}

NS_IMETHODIMP
UtilityProcessManager::Observer::Observe(nsISupports* aSubject,
                                         const char* aTopic,
                                         const char16_t* aData) {
  if (!strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID)) {
    mManager->OnXPCOMShutdown();
  } else if (!strcmp(aTopic, "nsPref:changed")) {
    mManager->OnPreferenceChange(aData);
  }
  return NS_OK;
}

void UtilityProcessManager::OnXPCOMShutdown() {
  LOGD("[%p] UtilityProcessManager::OnXPCOMShutdown", this);

  MOZ_ASSERT(NS_IsMainThread());
  sXPCOMShutdown = true;
  nsContentUtils::UnregisterShutdownObserver(mObserver);
  CleanShutdownAllProcesses();
}

void UtilityProcessManager::OnPreferenceChange(const char16_t* aData) {
  MOZ_ASSERT(NS_IsMainThread());
  if (NoMoreProcesses()) {
    return;
  }
  NS_LossyConvertUTF16toASCII strData(aData);

  mozilla::dom::Pref pref(strData,  false,
                           false, Nothing(), Nothing());
  Preferences::GetPreference(&pref, GeckoProcessType_Utility,
                              ""_ns);

  for (auto& p : mProcesses) {
    if (!p) {
      continue;
    }

    if (p->mProcessParent) {
      (void)p->mProcessParent->SendPreferenceUpdate(pref);
    } else if (IsProcessLaunching(p->mKind)) {
      p->mQueuedPrefs.AppendElement(pref);
    }
  }
}

RefPtr<UtilityProcessManager::ProcessFields> UtilityProcessManager::GetProcess(
    UtilityProcessKind aKind) {
  if (!mProcesses[aKind]) {
    return nullptr;
  }

  return mProcesses[aKind];
}

RefPtr<UtilityProcessManager::SharedLaunchPromise<Ok>>
UtilityProcessManager::LaunchProcess(UtilityProcessKind aKind) {
  LOGD("[%p] UtilityProcessManager::LaunchProcess UtilityProcessKind=%" PRIu64,
       this, aKind);
  using RetPromise = SharedLaunchPromise<Ok>;

  MOZ_ASSERT(NS_IsMainThread());

  if (IsShutdown()) {
    NS_WARNING("Reject early LaunchProcess() for Shutdown");
    return RetPromise::CreateAndReject(
        LaunchError("UPM::LaunchProcess(): IsShutdown()"), __func__);
  }

  RefPtr<ProcessFields> p = GetProcess(aKind);
  if (p && p->mNumProcessAttempts) {
    NS_WARNING("Reject LaunchProcess() for earlier mNumProcessAttempts");
    return RetPromise::CreateAndReject(
        LaunchError("UPM::LaunchProcess(): p->mNumProcessAttempts"), __func__);
  }

  if (p && p->mLaunchPromise && p->mProcess) {
    return p->mLaunchPromise;
  }

  if (!p) {
    p = new ProcessFields(aKind);
    mProcesses[aKind] = p;
  }

  geckoargs::ChildProcessArgs extraArgs;
  ProcessChild::AddPlatformBuildID(extraArgs);
  geckoargs::sUtilityProcessKind.Put(aKind, extraArgs);

  p->mProcess = new UtilityProcessHost(aKind, this);
  if (!p->mProcess->Launch(std::move(extraArgs))) {
    p->mNumProcessAttempts++;
    DestroyProcess(aKind);
    NS_WARNING("Reject LaunchProcess() for mNumProcessAttempts++");
    return RetPromise::CreateAndReject(
        LaunchError("UPM::LaunchProcess(): mNumProcessAttempts++"), __func__);
  }

  RefPtr<UtilityProcessManager> self = this;
  p->mLaunchPromise = p->mProcess->LaunchPromise()->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [self, p, aKind](Ok) -> RefPtr<RetPromise> {
        if (self->IsShutdown()) {
          NS_WARNING(
              "Reject LaunchProcess() after LaunchPromise() for Shutdown");
          return RetPromise::CreateAndReject(
              LaunchError("UPM::LaunchProcess(): post-await IsShutdown()"),
              __func__);
        }

        if (self->IsProcessDestroyed(aKind)) {
          NS_WARNING(
              "Reject LaunchProcess() after LaunchPromise() for destroyed "
              "process");
          return RetPromise::CreateAndReject(
              LaunchError(
                  "UPM::LaunchProcess(): post-await IsProcessDestroyed()"),
              __func__);
        }

        p->mProcessParent = p->mProcess->GetActor();

        for (const mozilla::dom::Pref& pref : p->mQueuedPrefs) {
          (void)NS_WARN_IF(!p->mProcessParent->SendPreferenceUpdate(pref));
        }
        p->mQueuedPrefs.Clear();

        return RetPromise::CreateAndResolve(Ok{}, __func__);
      },
      [self, p, aKind](LaunchError error) {
        if (GetSingleton()) {
          p->mNumProcessAttempts++;
          self->DestroyProcess(aKind);
        }
        NS_WARNING("Reject LaunchProcess() for LaunchPromise() rejection");
        return RetPromise::CreateAndReject(std::move(error), __func__);
      });

  return p->mLaunchPromise;
}

template <typename Actor>
RefPtr<UtilityProcessManager::LaunchPromise<Ok>>
UtilityProcessManager::StartUtility(RefPtr<Actor> aActor,
                                    UtilityProcessKind aKind) {
  using RetPromise = LaunchPromise<Ok>;

  LOGD(
      "[%p] UtilityProcessManager::StartUtility actor=%p "
      "UtilityProcessKind=%" PRIu64,
      this, aActor.get(), aKind);

  if (!aActor) {
    MOZ_ASSERT(false, "Actor singleton failure");
    return RetPromise::CreateAndReject(
        LaunchError("UPM::StartUtility: aActor is null"), __func__);
  }

  if (aActor->CanSend()) {
    return RetPromise::CreateAndResolve(Ok{}, __func__);
  }

  RefPtr<UtilityProcessManager> self = this;
  return LaunchProcess(aKind)->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [self, aActor, aKind]() -> RefPtr<RetPromise> {
        RefPtr<UtilityProcessParent> utilityParent =
            self->GetProcessParent(aKind);
        if (!utilityParent) {
          NS_WARNING("Missing parent in StartUtility");
          return RetPromise::CreateAndReject(
              LaunchError("UPM::GetProcessParent"), __func__);
        }

        if (!aActor->CanSend()) {
          if (!utilityParent->CanSend()) {
            NS_WARNING("Utility process died before IPC could be established");
            return RetPromise::CreateAndReject(
                LaunchError("UPM::UtilityParent died"), __func__);
          }

          nsresult rv = aActor->BindToUtilityProcess(utilityParent);
          if (NS_FAILED(rv)) {
            MOZ_ASSERT(false, "Protocol endpoints failure");
            return RetPromise::CreateAndReject(
                LaunchError("BindToUtilityProcess", rv), __func__);
          }

          MOZ_DIAGNOSTIC_ASSERT(aActor->CanSend(), "IPC established for actor");
          self->RegisterActor(utilityParent, aActor->GetActorName());
        }

        return RetPromise::CreateAndResolve(Ok{}, __func__);
      },
      [self](LaunchError const& error) {
        NS_WARNING("Reject StartUtility() for LaunchProcess() rejection");
        if (!self->IsShutdown()) {
          NS_WARNING("Reject StartUtility() when !IsShutdown()");
        }
        return RetPromise::CreateAndReject(error, __func__);
      });
}

RefPtr<UtilityProcessManager::StartRemoteDecodingUtilityPromise>
UtilityProcessManager::StartProcessForRemoteMediaDecoding(
    EndpointProcInfo aOtherProcess, dom::ContentParentId aChildId,
    UtilityProcessKind aKind) {
  using RetPromise = StartRemoteDecodingUtilityPromise;

  if (aKind != UtilityProcessKind::GENERIC_UTILITY) {
    return RetPromise::CreateAndReject(
        LaunchError("Start...MediaDecoding: bad utility process type"),
        __func__);
  }
  RefPtr<UtilityProcessManager> self = this;
  RefPtr<UtilityMediaServiceChild> umsc =
      UtilityMediaServiceChild::GetSingleton(aKind);
  MOZ_ASSERT(umsc, "Unable to get a singleton for UtilityMediaServiceChild");
  return StartUtility(umsc, aKind)
      ->Then(
          GetMainThreadSerialEventTarget(), __func__,
          [self, umsc, aOtherProcess, aChildId, aKind]() {
            RefPtr<UtilityProcessParent> parent =
                self->GetProcessParent(aKind);
            if (!parent) {
              NS_WARNING("UtilityMediaServiceParent lost in the middle");
              return RetPromise::CreateAndReject(
                  LaunchError("Start...MediaDecoding: parent lost"), __func__);
            }

            if (!umsc->CanSend()) {
              NS_WARNING("UtilityMediaServiceChild lost in the middle");
              return RetPromise::CreateAndReject(
                  LaunchError("Start...MediaDecoding: child lost"), __func__);
            }

            EndpointProcInfo process = parent->OtherEndpointProcInfo();

            Endpoint<PRemoteMediaManagerChild> childPipe;
            Endpoint<PRemoteMediaManagerParent> parentPipe;
            if (nsresult const rv = PRemoteMediaManager::CreateEndpoints(
                    process, aOtherProcess, &parentPipe, &childPipe);
                NS_FAILED(rv)) {
              MOZ_ASSERT(false, "Could not create content remote decoder");
              return RetPromise::CreateAndReject(
                  LaunchError("PRemoteMediaManager::CreateEndpoints", rv),
                  __func__);
            }

            if (!umsc->SendNewContentRemoteMediaManager(std::move(parentPipe),
                                                        aChildId)) {
              MOZ_ASSERT(false, "SendNewContentRemoteMediaManager failure");
              return RetPromise::CreateAndReject(
                  LaunchError("UMSC::SendNewCRDM"), __func__);
            }

            return RetPromise::CreateAndResolve(std::move(childPipe), __func__);
          },
          [](LaunchError&& error) {
            NS_WARNING(
                "Reject StartProcessForRemoteMediaDecoding() for "
                "StartUtility() rejection");
            return RetPromise::CreateAndReject(std::move(error), __func__);
          });
}

RefPtr<UtilityProcessManager::JSOraclePromise>
UtilityProcessManager::StartJSOracle(dom::JSOracleParent* aParent) {
  using RetPromise = JSOraclePromise;
  return StartUtility(RefPtr{aParent}, UtilityProcessKind::GENERIC_UTILITY)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          []() { return RetPromise::CreateAndResolve(true, __func__); },
          [](LaunchError const&) {
            return RetPromise::CreateAndReject(NS_ERROR_NOT_AVAILABLE,
                                               __func__);
          });
}


#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
RefPtr<UtilityProcessManager::PKCS11ModulePromise>
UtilityProcessManager::StartPKCS11Module() {
  using RetPromise = PKCS11ModulePromise;
  auto parent = MakeRefPtr<psm::PKCS11ModuleParent>();
  auto startPromise = StartUtility(parent, UtilityProcessKind::PKCS11_MODULE);
  return startPromise->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [parent = std::move(parent)]() mutable {
        if (!parent->CanSend()) {
          MOZ_ASSERT(false, "PKCS11ModuleParent lost in the middle");
          return RetPromise::CreateAndReject(
              LaunchError("StartPKCS11Module: !parent->CanSend()"),
              __PRETTY_FUNCTION__);
        }
        return RetPromise::CreateAndResolve(std::move(parent), __func__);
      },
      [](LaunchError&& aError) {
        MOZ_ASSERT_UNREACHABLE(
            "StartPKCS11Module: failure when starting actor");
        return RetPromise::CreateAndReject(std::move(aError), __func__);
      });
}
#endif

bool UtilityProcessManager::IsProcessLaunching(UtilityProcessKind aKind) {
  MOZ_ASSERT(NS_IsMainThread());

  RefPtr<ProcessFields> p = GetProcess(aKind);
  if (!p) {
    MOZ_CRASH("Cannot check process launching with no process");
    return false;
  }

  return p->mProcess && !(p->mProcessParent);
}

bool UtilityProcessManager::IsProcessDestroyed(UtilityProcessKind aKind) {
  MOZ_ASSERT(NS_IsMainThread());
  RefPtr<ProcessFields> p = GetProcess(aKind);
  if (!p) {
    MOZ_CRASH("Cannot check process destroyed with no process");
    return false;
  }
  return !p->mProcess && !p->mProcessParent;
}

void UtilityProcessManager::OnProcessUnexpectedShutdown(
    UtilityProcessHost* aHost) {
  MOZ_ASSERT(NS_IsMainThread());

  for (auto& it : mProcesses) {
    if (it && it->mProcess && it->mProcess == aHost) {
      it->mNumUnexpectedCrashes++;
      DestroyProcess(it->mKind);
      return;
    }
  }

  MOZ_CRASH(
      "Called UtilityProcessManager::OnProcessUnexpectedShutdown with invalid "
      "aHost");
}

void UtilityProcessManager::CleanShutdownAllProcesses() {
  LOGD("[%p] UtilityProcessManager::CleanShutdownAllProcesses", this);

  for (auto& it : mProcesses) {
    if (it) {
      DestroyProcess(it->mKind);
    }
  }
}

void UtilityProcessManager::CleanShutdown(UtilityProcessKind aKind) {
  LOGD("[%p] UtilityProcessManager::CleanShutdown UtilityProcessKind=%" PRIu64,
       this, aKind);

  DestroyProcess(aKind);
}

uint16_t UtilityProcessManager::AliveProcesses() {
  uint16_t alive = 0;
  for (auto& p : mProcesses) {
    if (p != nullptr) {
      alive++;
    }
  }
  return alive;
}

bool UtilityProcessManager::NoMoreProcesses() { return AliveProcesses() == 0; }

void UtilityProcessManager::DestroyProcess(UtilityProcessKind aKind) {
  LOGD("[%p] UtilityProcessManager::DestroyProcess UtilityProcessKind=%" PRIu64,
       this, aKind);

  MOZ_RELEASE_ASSERT(NS_IsMainThread());

  if (AliveProcesses() <= 1) {
    if (mObserver) {
      Preferences::RemoveObserver(mObserver, "");
    }

    mObserver = nullptr;
  }

  RefPtr<ProcessFields> p = GetProcess(aKind);
  if (!p) {
    return;
  }

  p->mQueuedPrefs.Clear();
  p->mProcessParent = nullptr;

  if (!p->mProcess) {
    return;
  }

  p->mProcess->Shutdown();
  p->mProcess = nullptr;

  mProcesses[aKind] = nullptr;

  if (NoMoreProcesses()) {
    sSingleton = nullptr;
  }
}

Maybe<base::ProcessId> UtilityProcessManager::ProcessPid(
    UtilityProcessKind aKind) {
  MOZ_ASSERT(NS_IsMainThread());
  RefPtr<ProcessFields> p = GetProcess(aKind);
  if (!p) {
    return Nothing();
  }
  if (p->mProcessParent) {
    return Some(p->mProcessParent->OtherPid());
  }
  return Nothing();
}

class UtilityMemoryReporter : public MemoryReportingProcess {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(UtilityMemoryReporter, override)

  explicit UtilityMemoryReporter(UtilityProcessParent* aParent) {
    mParent = aParent;
  }

  bool IsAlive() const override { return bool(GetParent()); }

  bool SendRequestMemoryReport(
      const uint32_t& aGeneration, const bool& aAnonymize,
      const bool& aMinimizeMemoryUsage,
      const Maybe<ipc::FileDescriptor>& aDMDFile) override {
    RefPtr<UtilityProcessParent> parent = GetParent();
    if (!parent) {
      return false;
    }

    return parent->SendRequestMemoryReport(aGeneration, aAnonymize,
                                           aMinimizeMemoryUsage, aDMDFile);
  }

  int32_t Pid() const override {
    if (RefPtr<UtilityProcessParent> parent = GetParent()) {
      return (int32_t)parent->OtherPid();
    }
    return 0;
  }

 private:
  RefPtr<UtilityProcessParent> GetParent() const { return mParent; }

  RefPtr<UtilityProcessParent> mParent = nullptr;

 protected:
  ~UtilityMemoryReporter() = default;
};

RefPtr<MemoryReportingProcess> UtilityProcessManager::GetProcessMemoryReporter(
    UtilityProcessParent* parent) {
  return new UtilityMemoryReporter(parent);
}

}  
