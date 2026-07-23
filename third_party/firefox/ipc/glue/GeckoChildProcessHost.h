/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(IPC_GLUE_GECKOCHILDPROCESSHOST_H_)
#define IPC_GLUE_GECKOCHILDPROCESSHOST_H_

#include "base/file_path.h"
#include "base/process_util.h"
#include "base/waitable_event.h"
#include "chrome/common/ipc_message.h"
#include "mojo/core/ports/port_ref.h"

#include "mozilla/GeckoArgs.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/FileDescriptor.h"
#include "mozilla/ipc/NodeChannel.h"
#include "mozilla/ipc/LaunchError.h"
#include "mozilla/ipc/ScopedPort.h"
#include "mozilla/ipc/UtilityProcessTypes.h"
#include "mozilla/Atomics.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Monitor.h"
#include "mozilla/MozPromise.h"
#include "mozilla/RWLock.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/UniquePtr.h"

#include "nsCOMPtr.h"
#include "nsXULAppAPI.h"  // for GeckoProcessType
#include "nsString.h"




#if (0 && defined(_ARM64_)) || \
    (0 && defined(__aarch64__))
#  define ALLOW_GECKO_CHILD_PROCESS_ARCH
#endif

struct _MacSandboxInfo;
typedef _MacSandboxInfo MacSandboxInfo;

namespace mozilla {
namespace ipc {

typedef mozilla::MozPromise<base::ProcessHandle, LaunchError, false>
    ProcessHandlePromise;

class GeckoChildProcessHost : public SupportsWeakPtr,
                              public LinkedListElement<GeckoChildProcessHost> {
 protected:
  typedef mozilla::Monitor Monitor;
  typedef std::vector<std::string> StringVector;

 public:
  using ProcessId = base::ProcessId;
  using ProcessHandle = base::ProcessHandle;

  explicit GeckoChildProcessHost(GeckoProcessType aProcessType,
                                 bool aIsFileContent = false);

  void Destroy();

  static uint32_t GetUniqueID();

  void SetEnv(const char* aKey, const char* aValue);

  bool AsyncLaunch(
      geckoargs::ChildProcessArgs aExtraOpts = geckoargs::ChildProcessArgs{});

  virtual bool WaitUntilConnected(int32_t aTimeoutMs = 0);

  bool LaunchAndWaitForProcessHandle(
      geckoargs::ChildProcessArgs aExtraOpts = geckoargs::ChildProcessArgs());
  bool WaitForProcessHandle();

  bool SyncLaunch(
      geckoargs::ChildProcessArgs aExtraOpts = geckoargs::ChildProcessArgs(),
      int32_t timeoutMs = 0);

  virtual void OnChannelConnected(base::ProcessId peer_pid);

  RefPtr<ProcessHandlePromise> WhenProcessHandleReady();

  bool InitializeChannel(IPC::Channel::ChannelHandle* aClientHandle);

  virtual bool CanShutdown() { return true; }

  UntypedEndpoint TakeInitialEndpoint() {
    return UntypedEndpoint{PrivateIPDLInterface{}, std::move(mInitialPort),
                           mInitialChannelId, EndpointProcInfo::Current(),
                           EndpointProcInfo{.mPid = GetChildProcessId(),
                                            .mChildID = GetChildID()}};
  }

  ProcessHandle GetChildProcessHandle();

  ProcessId GetChildProcessId();

  GeckoChildID GetChildID() const { return mChildID; }

  GeckoProcessType GetProcessType() { return mProcessType; }


#if defined(ALLOW_GECKO_CHILD_PROCESS_ARCH)
  void SetLaunchArchitecture(uint32_t aArch) { mLaunchArch = aArch; }
#endif

  void SetAlreadyDead();

  typedef std::function<void(GeckoChildProcessHost*)> GeckoProcessCallback;

  static void GetAll(const GeckoProcessCallback& aCallback);

  friend class BaseProcessLauncher;
  friend class PosixProcessLauncher;
  friend class WindowsProcessLauncher;

 protected:
  virtual ~GeckoChildProcessHost();
  GeckoProcessType mProcessType;
  GeckoChildID mChildID;
  bool mIsFileContent;
  mutable Monitor mMonitor;
  FilePath mProcessPath;
#if defined(ALLOW_GECKO_CHILD_PROCESS_ARCH)
  uint32_t mLaunchArch = base::PROCESS_ARCH_INVALID;
#endif
  UniquePtr<base::LaunchOptions> mLaunchOptions;
  ScopedPort mInitialPort;
  nsID mInitialChannelId;
  RefPtr<NodeController> mNodeController;
  RefPtr<NodeChannel> mNodeChannel;

  enum {
    CREATING_CHANNEL = 0,
    CHANNEL_INITIALIZED,
    PROCESS_CREATED,
    PROCESS_CONNECTED,
    PROCESS_ERROR
  } mProcessState MOZ_GUARDED_BY(mMonitor);

  bool PrepareLaunch(geckoargs::ChildProcessArgs& aExtraOpts);


  UtilityProcessKind mUtilityKind;

  mozilla::RWLock mHandleLock;
  ProcessHandle mChildProcessHandle MOZ_GUARDED_BY(mHandleLock);
  RefPtr<ProcessHandlePromise> mHandlePromise;


  bool OpenPrivilegedHandle(base::ProcessId aPid) MOZ_REQUIRES(mHandleLock);


  virtual void OnProcessLaunchError(const LaunchError aError);

 private:
  DISALLOW_EVIL_CONSTRUCTORS(GeckoChildProcessHost);

  void RemoveFromProcessList();

  nsCOMPtr<nsIFile> mProfileDir;

  mozilla::Atomic<bool> mDestroying;

  static uint32_t sNextUniqueID;
  static StaticAutoPtr<LinkedList<GeckoChildProcessHost>>
      sGeckoChildProcessHosts MOZ_GUARDED_BY(sMutex);
  static StaticMutex sMutex;
};

nsCOMPtr<nsISerialEventTarget> GetIPCLauncher();

} 
} 

#endif
