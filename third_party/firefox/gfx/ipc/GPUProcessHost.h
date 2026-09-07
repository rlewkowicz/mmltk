/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _include_mozilla_gfx_ipc_GPUProcessHost_h_
#define _include_mozilla_gfx_ipc_GPUProcessHost_h_

#include "mozilla/UniquePtr.h"
#include "mozilla/gfx/Types.h"
#include "mozilla/ipc/GeckoChildProcessHost.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/media/MediaUtils.h"


namespace mozilla {
namespace ipc {
class SharedPreferenceSerializer;
}
}  
class nsITimer;

namespace mozilla {
namespace gfx {

class GPUChild;

class GPUProcessHost final : public mozilla::ipc::GeckoChildProcessHost {
  friend class GPUChild;

 public:
  class Listener {
   public:
    virtual void OnProcessLaunchComplete(GPUProcessHost* aHost) {}

    virtual void OnProcessUnexpectedShutdown(GPUProcessHost* aHost) {}

    virtual void OnRemoteProcessDeviceReset(
        GPUProcessHost* aHost, const DeviceResetReason& aReason,
        const DeviceResetDetectPlace& aPlace) {}

    virtual void OnProcessDeclaredStable() {}
  };

  explicit GPUProcessHost(Listener* listener);

  bool Launch(geckoargs::ChildProcessArgs aExtraOpts);

  bool WaitForLaunch();

  void Shutdown(bool aUnexpectedShutdown = false);

  GPUChild* GetActor() const { return mGPUChild.get(); }

  uint64_t GetProcessToken() const;

  bool IsConnected() const { return !!mGPUChild; }

  bool IsLaunchOomError() const {
    MonitorAutoLock lock(mMonitor);
    return mLaunchOomError;
  }

  TimeStamp GetLaunchTime() const { return mLaunchTime; }

  void OnChannelConnected(base::ProcessId peer_pid) override;

  void SetListener(Listener* aListener);

  void KillProcess();

  void CrashProcess();



 private:
  ~GPUProcessHost();

  void InitAfterConnect(bool aSucceeded);
  void OnAsyncInitComplete();
  bool CompleteInitSynchronously();

  void OnProcessLaunchError(const base::LaunchError aError) override;

  void OnChannelClosed();

  void KillHard();

  void DestroyProcess();


  DISALLOW_COPY_AND_ASSIGN(GPUProcessHost);

  Listener* mListener;

  enum class LaunchPhase { Unlaunched, Waiting, Connected, Complete };
  LaunchPhase mLaunchPhase;

  RefPtr<GPUChild> mGPUChild;
  uint64_t mProcessToken;

  UniquePtr<mozilla::ipc::SharedPreferenceSerializer> mPrefSerializer;

  bool mShutdownRequested;
  bool mChannelClosed;
  bool mLaunchOomError MOZ_GUARDED_BY(mMonitor) = false;

  TimeStamp mLaunchTime;

  const RefPtr<media::Refcountable<bool>> mLiveToken;

};

}  
}  

#endif  // _include_mozilla_gfx_ipc_GPUProcessHost_h_
