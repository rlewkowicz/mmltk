/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(_include_ipc_glue_UtilityProcessManager_h_)
#define _include_ipc_glue_UtilityProcessManager_h_
#include "mozilla/MozPromise.h"
#include "mozilla/dom/ipc/IdType.h"
#include "mozilla/ipc/UtilityProcessHost.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/ProcInfo.h"
#include "nsIObserver.h"
#include "nsTArray.h"

#include "mozilla/PRemoteMediaManagerChild.h"

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
#  include "mozilla/psm/PKCS11ModuleParent.h"
#endif

namespace mozilla {

class MemoryReportingProcess;

namespace dom {
class JSOracleParent;
class WindowsUtilsParent;
}  

namespace widget::filedialog {
class ProcessProxy;
}  

namespace ipc {

class UtilityProcessParent;

class UtilityProcessManager final : public UtilityProcessHost::Listener {
  friend class UtilityProcessParent;

 public:
  template <typename T>
  using LaunchPromise = MozPromise<T, LaunchError, true>;
  template <typename T>
  using SharedLaunchPromise = MozPromise<T, LaunchError, false>;

  using StartRemoteDecodingUtilityPromise =
      LaunchPromise<Endpoint<PRemoteMediaManagerChild>>;
  using JSOraclePromise = GenericNonExclusivePromise;


#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
  using PKCS11ModulePromise = LaunchPromise<RefPtr<psm::PKCS11ModuleParent>>;
#endif

  static RefPtr<UtilityProcessManager> GetSingleton();

  static RefPtr<UtilityProcessManager> GetIfExists();

  RefPtr<SharedLaunchPromise<Ok>> LaunchProcess(UtilityProcessKind aKind);

  template <typename Actor>
  RefPtr<LaunchPromise<Ok>> StartUtility(RefPtr<Actor> aActor,
                                         UtilityProcessKind aKind);

  RefPtr<StartRemoteDecodingUtilityPromise> StartProcessForRemoteMediaDecoding(
      EndpointProcInfo aOtherProcess, dom::ContentParentId aChildId,
      UtilityProcessKind aKind);

  RefPtr<JSOraclePromise> StartJSOracle(mozilla::dom::JSOracleParent* aParent);


#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
  RefPtr<PKCS11ModulePromise> StartPKCS11Module();
#endif

  void OnProcessUnexpectedShutdown(UtilityProcessHost* aHost);

  Maybe<base::ProcessId> ProcessPid(UtilityProcessKind aKind);

  RefPtr<MemoryReportingProcess> GetProcessMemoryReporter(
      UtilityProcessParent* parent);

  RefPtr<UtilityProcessParent> GetProcessParent(UtilityProcessKind aKind) {
    RefPtr<ProcessFields> p = GetProcess(aKind);
    if (!p) {
      return nullptr;
    }
    return p->mProcessParent;
  }

  nsTArray<RefPtr<UtilityProcessParent>> GetAllProcessesProcessParent() {
    nsTArray<RefPtr<UtilityProcessParent>> rv;
    for (auto& p : mProcesses) {
      if (p && p->mProcessParent) {
        rv.AppendElement(p->mProcessParent);
      }
    }
    return rv;
  }

  UtilityProcessHost* Process(UtilityProcessKind aKind) {
    RefPtr<ProcessFields> p = GetProcess(aKind);
    if (!p) {
      return nullptr;
    }
    return p->mProcess;
  }

  void RegisterActor(const RefPtr<UtilityProcessParent>& aParent,
                     UtilityActorName aActorName) {
    for (auto& p : mProcesses) {
      if (p && p->mProcessParent && p->mProcessParent == aParent) {
        p->mActors.AppendElement(aActorName);
        return;
      }
    }
  }

  Span<const UtilityActorName> GetActors(
      const RefPtr<UtilityProcessParent>& aParent) {
    for (auto& p : mProcesses) {
      if (p && p->mProcessParent && p->mProcessParent == aParent) {
        return p->mActors;
      }
    }
    return {};
  }

  Span<const UtilityActorName> GetActors(GeckoChildProcessHost* aHost) {
    for (auto& p : mProcesses) {
      if (p && p->mProcess == aHost) {
        return p->mActors;
      }
    }
    return {};
  }

  Span<const UtilityActorName> GetActors(UtilityProcessKind aKind) {
    auto proc = GetProcess(aKind);
    if (!proc) {
      return {};
    }
    return proc->mActors;
  }

  void CleanShutdown(UtilityProcessKind aKind);

  void CleanShutdownAllProcesses();

  uint16_t AliveProcesses();

 private:
  ~UtilityProcessManager();

  bool IsProcessLaunching(UtilityProcessKind aKind);
  bool IsProcessDestroyed(UtilityProcessKind aKind);

  void OnXPCOMShutdown();
  void OnPreferenceChange(const char16_t* aData);

  UtilityProcessManager();

  void Init();

  void DestroyProcess(UtilityProcessKind aKind);

  bool IsShutdown() const;

  class Observer final : public nsIObserver {
   public:
    NS_DECL_ISUPPORTS
    NS_DECL_NSIOBSERVER
    explicit Observer(UtilityProcessManager* aManager);

   protected:
    ~Observer() = default;

    RefPtr<UtilityProcessManager> mManager;
  };
  friend class Observer;

  RefPtr<Observer> mObserver;

  class ProcessFields final {
   public:
    NS_INLINE_DECL_THREADSAFE_REFCOUNTING(ProcessFields);

    explicit ProcessFields(UtilityProcessKind aKind) : mKind(aKind) {};

    RefPtr<SharedLaunchPromise<Ok>> mLaunchPromise;

    uint32_t mNumProcessAttempts = 0;
    uint32_t mNumUnexpectedCrashes = 0;

    UtilityProcessHost* mProcess = nullptr;
    RefPtr<UtilityProcessParent> mProcessParent = nullptr;

    nsTArray<dom::Pref> mQueuedPrefs;

    nsTArray<UtilityActorName> mActors;

    UtilityProcessKind mKind = UtilityProcessKind::COUNT;

   protected:
    ~ProcessFields() = default;
  };

  EnumeratedArray<UtilityProcessKind, RefPtr<ProcessFields>,
                  size_t(UtilityProcessKind::COUNT)>
      mProcesses;

  RefPtr<ProcessFields> GetProcess(UtilityProcessKind);
  bool NoMoreProcesses();

};

}  

}  

#endif
