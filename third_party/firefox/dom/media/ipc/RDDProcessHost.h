/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _include_dom_media_ipc_RDDProcessHost_h_
#define _include_dom_media_ipc_RDDProcessHost_h_
#include "mozilla/UniquePtr.h"
#include "mozilla/ipc/GeckoChildProcessHost.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/media/MediaUtils.h"

namespace mozilla::ipc {
class SharedPreferenceSerializer;
}  
class nsITimer;

namespace mozilla {

class RDDChild;

class RDDProcessHost final : public mozilla::ipc::GeckoChildProcessHost {
  friend class RDDChild;

 public:
  class Listener {
   public:
    virtual void OnProcessUnexpectedShutdown(RDDProcessHost* aHost) {}
  };

  explicit RDDProcessHost(Listener* listener);

  bool Launch(geckoargs::ChildProcessArgs aExtraOpts);

  RefPtr<GenericNonExclusivePromise> LaunchPromise();

  void Shutdown();

  RDDChild* GetActor() const {
    MOZ_ASSERT(NS_IsMainThread());
    return mRDDChild.get();
  }

  uint64_t GetProcessToken() const;

  bool IsConnected() const {
    MOZ_ASSERT(NS_IsMainThread());
    return !!mRDDChild;
  }

  TimeStamp GetLaunchTime() const { return mLaunchTime; }

  void OnChannelConnected(base::ProcessId peer_pid) override;

  void SetListener(Listener* aListener);


 private:
  ~RDDProcessHost();

  void InitAfterConnect(bool aSucceeded);

  void OnChannelClosed();

  void KillHard(const char* aReason);

  void DestroyProcess();


  DISALLOW_COPY_AND_ASSIGN(RDDProcessHost);

  Listener* const mListener;

  enum class LaunchPhase { Unlaunched, Waiting, Complete };
  LaunchPhase mLaunchPhase = LaunchPhase::Unlaunched;

  RefPtr<RDDChild> mRDDChild;
  uint64_t mProcessToken = 0;

  UniquePtr<ipc::SharedPreferenceSerializer> mPrefSerializer;

  bool mShutdownRequested = false;
  bool mChannelClosed = false;

  TimeStamp mLaunchTime;
  void RejectPromise();
  void ResolvePromise();

  const RefPtr<media::Refcountable<bool>> mLiveToken;
  RefPtr<GenericNonExclusivePromise::Private> mLaunchPromise;
  bool mLaunchPromiseSettled = false;
  bool mTimerChecked = false;
};

}  

#endif  // _include_dom_media_ipc_RDDProcessHost_h_
