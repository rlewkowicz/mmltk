/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _include_ipc_glue_UtilityProcessHost_h_
#define _include_ipc_glue_UtilityProcessHost_h_

#include "mozilla/UniquePtr.h"
#include "mozilla/ipc/UtilityProcessParent.h"
#include "mozilla/ipc/UtilityProcessTypes.h"
#include "mozilla/ipc/GeckoChildProcessHost.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/media/MediaUtils.h"
#include "mozilla/ipc/ProcessUtils.h"


namespace mozilla::ipc {

class UtilityProcessParent;

class UtilityProcessHost final : public mozilla::ipc::GeckoChildProcessHost {
  friend class UtilityProcessParent;

 public:
  class Listener {
   protected:
    virtual ~Listener() = default;

   public:
    NS_INLINE_DECL_THREADSAFE_REFCOUNTING(UtilityProcessHost::Listener);

    virtual void OnProcessUnexpectedShutdown(UtilityProcessHost* aHost) {}
  };

  explicit UtilityProcessHost(UtilityProcessKind aKind,
                              RefPtr<Listener> listener);

  bool Launch(geckoargs::ChildProcessArgs aExtraOpts);

  using LaunchPromiseType = MozPromise<Ok, LaunchError, false>;
  RefPtr<LaunchPromiseType> LaunchPromise();

  void Shutdown();

  RefPtr<UtilityProcessParent> GetActor() const {
    MOZ_ASSERT(NS_IsMainThread());
    return mUtilityProcessParent;
  }

  bool IsConnected() const {
    MOZ_ASSERT(NS_IsMainThread());
    return bool(mUtilityProcessParent);
  }

  void OnChannelConnected(base::ProcessId peer_pid) override;


 private:
  ~UtilityProcessHost();

  void InitAfterConnect(bool aSucceeded);

  void OnChannelClosed(IProtocol::ActorDestroyReason aReason);

  void KillHard(const char* aReason);

  void DestroyProcess();


  DISALLOW_COPY_AND_ASSIGN(UtilityProcessHost);

  RefPtr<Listener> mListener;

  enum class LaunchPhase { Unlaunched, Waiting, Complete };
  LaunchPhase mLaunchPhase = LaunchPhase::Unlaunched;

  RefPtr<UtilityProcessParent> mUtilityProcessParent;

  UniquePtr<ipc::SharedPreferenceSerializer> mPrefSerializer{};

  bool mShutdownRequested = false;

  void ResolvePromise();
  void RejectPromise(LaunchError);

  const RefPtr<media::Refcountable<bool>> mLiveToken;

  RefPtr<LaunchPromiseType::Private> mLaunchPromise{};
  bool mLaunchPromiseSettled = false;
  bool mLaunchPromiseLaunched = false;
  bool mLaunchCompleted = false;

};

}  

#endif  // _include_ipc_glue_UtilityProcessHost_h_
