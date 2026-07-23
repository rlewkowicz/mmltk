/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GeckoChildProcessHost.h"

#include "base/command_line.h"
#include "base/process.h"
#include "base/process_util.h"
#include "base/string_util.h"
#include "base/task.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/process_watcher.h"
#include "MainThreadUtils.h"
#include "mozilla/Preferences.h"
#include "mozilla/Sprintf.h"
#include "nsXPCOMPrivate.h"
#include "prenv.h"
#include "prerror.h"


#include "ProtocolUtils.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Logging.h"
#include "mozilla/Maybe.h"
#include "mozilla/GeckoArgs.h"
#include "mozilla/Omnijar.h"
#include "mozilla/RDDProcessHost.h"
#include "mozilla/Services.h"
#include "mozilla/SharedThreadPool.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/TaskQueue.h"
#include "mozilla/UniquePtrExtensions.h"
#include "mozilla/ipc/IOThread.h"
#include "mozilla/ipc/EnvironmentMap.h"
#include "mozilla/ipc/NodeController.h"
#include "mozilla/net/SocketProcessHost.h"
#include "nsDirectoryService.h"
#include "nsDirectoryServiceDefs.h"
#include "nsIFile.h"
#include "nsIObserverService.h"
#include "nsPrintfCString.h"




#include "mozilla/ipc/UtilityProcessHost.h"
#include "mozilla/ipc/UtilityProcessTypes.h"

#include "nsClassHashtable.h"
#include "nsHashKeys.h"
#include "nsNativeCharsetUtils.h"
#include "nsTArray.h"
#include "nscore.h"  // for NS_FREE_PERMANENT_DATA
#include "nsIThread.h"

using mozilla::MonitorAutoLock;
using mozilla::Preferences;
using mozilla::StaticMutexAutoLock;


#if defined(MOZ_ENABLE_FORKSERVER)
#  include "mozilla/ipc/ForkServiceChild.h"
#endif

static bool ShouldHaveDirectoryService() {
  return GeckoProcessType_Default == XRE_GetProcessType();
}


namespace mozilla {
namespace ipc {

struct LaunchResults {
  base::ProcessHandle mHandle = 0;
};
typedef mozilla::MozPromise<LaunchResults, LaunchError, true>
    ProcessLaunchPromise;

static Atomic<int32_t> gChildCounter;

class BaseProcessLauncher {
 public:
  BaseProcessLauncher(GeckoChildProcessHost* aHost,
                      geckoargs::ChildProcessArgs&& aExtraOpts)
      : mProcessType(aHost->mProcessType),
        mLaunchOptions(std::move(aHost->mLaunchOptions)),
        mChildArgs(std::move(aExtraOpts)),
        mUtilityKind(aHost->mUtilityKind)
  {
    aHost->mInitialChannelId.ToProvidedString(mInitialChannelIdString);
    mChildID = aHost->mChildID;
    SprintfLiteral(mChildIDString, "%d", aHost->mChildID);

    mLaunchThread = GetIPCLauncher();

    if (ShouldHaveDirectoryService()) {
      (void)nsDirectoryService::gService->GetCurrentProcessDirectory(
          getter_AddRefs(mAppDir));
    }
  }

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(BaseProcessLauncher);

#if defined(ALLOW_GECKO_CHILD_PROCESS_ARCH)
  void SetLaunchArchitecture(uint32_t aLaunchArch) {
    mLaunchArch = aLaunchArch;
  }
#endif

  RefPtr<ProcessLaunchPromise> Launch(GeckoChildProcessHost*);

 protected:
  virtual ~BaseProcessLauncher() = default;

  RefPtr<ProcessLaunchPromise> PerformAsyncLaunch();

  virtual Result<Ok, LaunchError> DoSetup();
  virtual RefPtr<ProcessLaunchPromise> DoLaunch() = 0;

  void MapChildLogging();

  static BinPathType GetPathToBinary(FilePath&, GeckoProcessType,
                                     UtilityProcessKind utilityKind);

  void GetChildLogName(const char* origLogName, nsACString& buffer);

  const char* ChildProcessType() {
    return XRE_GeckoProcessTypeToString(mProcessType);
  }

  nsCOMPtr<nsISerialEventTarget> mLaunchThread;
  GeckoProcessType mProcessType;
  UniquePtr<base::LaunchOptions> mLaunchOptions;
#if defined(ALLOW_GECKO_CHILD_PROCESS_ARCH)
  uint32_t mLaunchArch = base::PROCESS_ARCH_INVALID;
#endif
  geckoargs::ChildProcessArgs mChildArgs;
  UtilityProcessKind mUtilityKind;
  LaunchResults mResults = LaunchResults();
  char mInitialChannelIdString[NSID_LENGTH];
  GeckoChildID mChildID;
  char mChildIDString[32];

  nsCOMPtr<nsIFile> mAppDir;
};


#if defined(XP_UNIX)
class PosixProcessLauncher : public BaseProcessLauncher {
 public:
  PosixProcessLauncher(GeckoChildProcessHost* aHost,
                       geckoargs::ChildProcessArgs&& aExtraOpts)
      : BaseProcessLauncher(aHost, std::move(aExtraOpts)),
        mProfileDir(aHost->mProfileDir) {}

 protected:
  virtual Result<Ok, LaunchError> DoSetup() override;
  virtual RefPtr<ProcessLaunchPromise> DoLaunch() override;

  nsCOMPtr<nsIFile> mProfileDir;
};

#if defined(MOZ_WIDGET_GTK)
class LinuxProcessLauncher : public PosixProcessLauncher {
 public:
  LinuxProcessLauncher(GeckoChildProcessHost* aHost,
                       geckoargs::ChildProcessArgs&& aExtraOpts)
      : PosixProcessLauncher(aHost, std::move(aExtraOpts)) {}

 protected:
  virtual Result<Ok, LaunchError> DoSetup() override;
};
typedef LinuxProcessLauncher ProcessLauncher;
#else
#    error "Unknown platform"
#endif
#endif

using base::ProcessHandle;
using mozilla::ipc::BaseProcessLauncher;
using mozilla::ipc::ProcessLauncher;

mozilla::StaticAutoPtr<mozilla::LinkedList<GeckoChildProcessHost>>
    GeckoChildProcessHost::sGeckoChildProcessHosts;

mozilla::StaticMutex GeckoChildProcessHost::sMutex;

GeckoChildProcessHost::GeckoChildProcessHost(GeckoProcessType aProcessType,
                                             bool aIsFileContent)
    : mProcessType(aProcessType),
      mChildID(++gChildCounter),
      mIsFileContent(aIsFileContent),
      mMonitor("mozilla.ipc.GeckoChildProcessHost.mMonitor"),
      mLaunchOptions(MakeUnique<base::LaunchOptions>()),
      mInitialChannelId(nsID::GenerateUUID()),
      mProcessState(CREATING_CHANNEL),
      mHandleLock("mozilla.ipc.GeckoChildProcessHost.mHandleLock"),
      mChildProcessHandle(0),
      mDestroying(false) {
  MOZ_COUNT_CTOR(GeckoChildProcessHost);
  MOZ_RELEASE_ASSERT(mChildID > 0, "gChildCounter overflowed");
  StaticMutexAutoLock lock(sMutex);
  if (!sGeckoChildProcessHosts) {
    sGeckoChildProcessHosts = new mozilla::LinkedList<GeckoChildProcessHost>();
  }
  sGeckoChildProcessHosts->insertBack(this);
}

GeckoChildProcessHost::~GeckoChildProcessHost() {
  AssertIOThread();
  MOZ_RELEASE_ASSERT(mDestroying);

  MOZ_COUNT_DTOR(GeckoChildProcessHost);

  {
    mozilla::AutoWriteLock hLock(mHandleLock);

    if (mChildProcessHandle != 0) {
      ProcessWatcher::EnsureProcessTerminated(mChildProcessHandle);
      mChildProcessHandle = 0;
    }
  }
}

base::ProcessHandle GeckoChildProcessHost::GetChildProcessHandle() {
  mozilla::AutoReadLock handleLock(mHandleLock);
  return mChildProcessHandle;
}

base::ProcessId GeckoChildProcessHost::GetChildProcessId() {
  mozilla::AutoReadLock handleLock(mHandleLock);
  if (!mChildProcessHandle) {
    return 0;
  }
  return base::GetProcId(mChildProcessHandle);
}


void GeckoChildProcessHost::RemoveFromProcessList() {
  StaticMutexAutoLock lock(sMutex);
  if (!sGeckoChildProcessHosts) {
    return;
  }
  LinkedListElement<GeckoChildProcessHost>::removeFrom(
      *sGeckoChildProcessHosts);
}

void GeckoChildProcessHost::Destroy() {
  MOZ_RELEASE_ASSERT(!mDestroying);
  RemoveFromProcessList();
  RefPtr<ProcessHandlePromise> whenReady = mHandlePromise;

  if (!whenReady) {
    whenReady = ProcessHandlePromise::CreateAndReject(
        LaunchError("DestroyEarly"), __func__);
  }

  using Value = ProcessHandlePromise::ResolveOrRejectValue;
  mDestroying = true;

  MessageLoop* loop = MessageLoop::current();
  if (loop && MessageLoop::TYPE_IO == loop->type() &&
      !loop->IsAcceptingTasks()) {
    delete this;
    return;
  }

  whenReady->Then(XRE_GetAsyncIOEventTarget(), __func__,
                  [this](const Value&) { delete this; });
}

mozilla::BinPathType BaseProcessLauncher::GetPathToBinary(
    FilePath& exePath, GeckoProcessType processType,
    UtilityProcessKind utilityKind) {
  exePath = {};
  BinPathType pathType = XRE_GetChildProcBinPathType(processType);

  if (pathType == BinPathType::Self) {
    exePath = FilePath(CommandLine::ForCurrentProcess()->argv()[0]);
    return pathType;
  }


  if (ShouldHaveDirectoryService()) {
    MOZ_ASSERT(gGREBinPath);
    nsCString path;
    if (NS_SUCCEEDED(
            NS_CopyUnicodeToNative(nsDependentString(gGREBinPath), path))) {
      exePath = FilePath(path.get());
    }
  }

  if (exePath.empty()) {
    exePath = FilePath(CommandLine::ForCurrentProcess()->argv()[0]);
    exePath = exePath.DirName();
  }

  exePath = exePath.AppendASCII(MOZ_CHILD_PROCESS_NAME);

  return pathType;
}


uint32_t GeckoChildProcessHost::sNextUniqueID = 1;

uint32_t GeckoChildProcessHost::GetUniqueID() { return sNextUniqueID++; }

void GeckoChildProcessHost::SetEnv(const char* aKey, const char* aValue) {
  MOZ_ASSERT(mLaunchOptions);
  mLaunchOptions->env_map[ENVIRONMENT_STRING(aKey)] =
      ENVIRONMENT_STRING(aValue);
}

bool GeckoChildProcessHost::PrepareLaunch(
    geckoargs::ChildProcessArgs& aExtraOpts) {


  return true;
}


bool GeckoChildProcessHost::SyncLaunch(geckoargs::ChildProcessArgs aExtraOpts,
                                       int aTimeoutMs) {
  if (!AsyncLaunch(std::move(aExtraOpts))) {
    return false;
  }
  return WaitUntilConnected(aTimeoutMs);
}

bool GeckoChildProcessHost::AsyncLaunch(
    geckoargs::ChildProcessArgs aExtraOpts) {
  if (!PrepareLaunch(aExtraOpts)) {
    return false;
  }


  RefPtr<BaseProcessLauncher> launcher =
      MakeRefPtr<ProcessLauncher>(this, std::move(aExtraOpts));
#if defined(ALLOW_GECKO_CHILD_PROCESS_ARCH)
  launcher->SetLaunchArchitecture(mLaunchArch);
#endif

  MOZ_ASSERT(mHandlePromise == nullptr);
  mHandlePromise =
      mozilla::InvokeAsync<GeckoChildProcessHost*>(
          XRE_GetAsyncIOEventTarget(), launcher.get(), __func__,
          &BaseProcessLauncher::Launch, this)
          ->Then(
              XRE_GetAsyncIOEventTarget(), __func__,
              [this](LaunchResults&& aResults) {
                {
                  {
                    mozilla::AutoWriteLock handleLock(mHandleLock);
                    if (!OpenPrivilegedHandle(base::GetProcId(aResults.mHandle))
                    ) {
                      MOZ_CRASH("cannot open handle to child process");
                    }
                    base::CloseProcessHandle(aResults.mHandle);
                    aResults.mHandle = 0;


                    if (mNodeChannel) {
                      mNodeChannel->SetOtherPid(
                          base::GetProcId(this->mChildProcessHandle));
                    }
                  }



                  MonitorAutoLock lock(mMonitor);
                  if (mProcessState < PROCESS_CREATED) {
                    mProcessState = PROCESS_CREATED;
                  }
                  lock.Notify();
                }
                return ProcessHandlePromise::CreateAndResolve(
                    GetChildProcessHandle(), __func__);
              },
              [this](const LaunchError aError) {
                CHROMIUM_LOG(ERROR)
                    << "Failed to launch "
                    << XRE_GeckoProcessTypeToString(mProcessType)
                    << " subprocess @" << aError.FunctionName()
                    << " (Error:" << aError.ErrorCode() << ")";

                nsCString telemetryKey = nsPrintfCString(
                    "%s,%ld,%s",
                    aError.FunctionName().get(), aError.ErrorCode(),
                    XRE_GeckoProcessTypeToString(mProcessType));
                if (telemetryKey.Length() > 72) {
                  NS_WARNING(nsPrintfCString("Truncating telemetry key: %s",
                                             telemetryKey.get())
                                 .get());
                  telemetryKey.Truncate(72);
                }

                OnProcessLaunchError(aError);
                return ProcessHandlePromise::CreateAndReject(aError, __func__);
              });
  return true;
}

void GeckoChildProcessHost::OnProcessLaunchError(const LaunchError aError) {
  MonitorAutoLock lock(mMonitor);
  mProcessState = PROCESS_ERROR;
  lock.Notify();
}

bool GeckoChildProcessHost::WaitUntilConnected(int32_t aTimeoutMs) {

  TimeDuration timeout = (aTimeoutMs > 0)
                             ? TimeDuration::FromMilliseconds(aTimeoutMs)
                             : TimeDuration::Forever();

  MonitorAutoLock lock(mMonitor);
  TimeStamp waitStart = TimeStamp::Now();
  TimeStamp current;

  while (mProcessState != PROCESS_CONNECTED) {
    if (mProcessState == PROCESS_ERROR) {
      break;
    }

    CVStatus status = lock.Wait(timeout);
    if (status == CVStatus::Timeout) {
      break;
    }

    if (timeout != TimeDuration::Forever()) {
      current = TimeStamp::Now();
      timeout -= current - waitStart;
      waitStart = current;
    }
  }

  return mProcessState == PROCESS_CONNECTED;
}

bool GeckoChildProcessHost::WaitForProcessHandle() {
  MonitorAutoLock lock(mMonitor);
  while (mProcessState < PROCESS_CREATED) {
    lock.Wait();
  }
  MOZ_ASSERT(mProcessState == PROCESS_ERROR || GetChildProcessHandle());

  return mProcessState < PROCESS_ERROR;
}

bool GeckoChildProcessHost::LaunchAndWaitForProcessHandle(
    geckoargs::ChildProcessArgs aExtraOpts) {
  if (!AsyncLaunch(std::move(aExtraOpts))) {
    return false;
  }
  return WaitForProcessHandle();
}

bool GeckoChildProcessHost::InitializeChannel(
    IPC::Channel::ChannelHandle* aClientHandle) {
  mNodeController = NodeController::GetSingleton();
  if (!mNodeController->InviteChildProcess(this, aClientHandle, &mInitialPort,
                                           getter_AddRefs(mNodeChannel))) {
    return false;
  }

  MOZ_ASSERT(mInitialPort.IsValid());
  MOZ_ASSERT(mNodeChannel);

  MonitorAutoLock lock(mMonitor);
  mProcessState = CHANNEL_INITIALIZED;
  lock.Notify();
  return true;
}

void GeckoChildProcessHost::SetAlreadyDead() {
  mozilla::AutoWriteLock handleLock(mHandleLock);
  if (mChildProcessHandle &&
      mChildProcessHandle != base::kInvalidProcessHandle) {
    base::CloseProcessHandle(mChildProcessHandle);
  }

  mChildProcessHandle = 0;
}

void BaseProcessLauncher::GetChildLogName(const char* origLogName,
                                          nsACString& buffer) {
  {
    buffer.Append(origLogName);
  }

  static constexpr auto kMozLogExt = nsLiteralCString{MOZ_LOG_FILE_EXTENSION};
  if (StringEndsWith(buffer, kMozLogExt)) {
    buffer.Truncate(buffer.Length() - kMozLogExt.Length());
  }

  buffer.AppendLiteral(".child-");
  buffer.AppendASCII(mChildIDString);
}


static mozilla::StaticMutex gIPCLaunchThreadMutex;
static mozilla::StaticRefPtr<nsIThread> gIPCLaunchThread
    MOZ_GUARDED_BY(gIPCLaunchThreadMutex);

class IPCLaunchThreadObserver final : public nsIObserver {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER
 protected:
  virtual ~IPCLaunchThreadObserver() = default;
};

NS_IMPL_ISUPPORTS(IPCLaunchThreadObserver, nsIObserver, nsISupports)

NS_IMETHODIMP
IPCLaunchThreadObserver::Observe(nsISupports* aSubject, const char* aTopic,
                                 const char16_t* aData) {
  MOZ_RELEASE_ASSERT(strcmp(aTopic, "xpcom-shutdown-threads") == 0);

  nsCOMPtr<nsIThread> thread;
  {
    StaticMutexAutoLock lock(gIPCLaunchThreadMutex);
    thread = gIPCLaunchThread.forget();
  }

  nsresult rv = thread ? thread->Shutdown() : NS_OK;
  (void)NS_WARN_IF(NS_FAILED(rv));
  return rv;
}

nsCOMPtr<nsISerialEventTarget> GetIPCLauncher() {
  StaticMutexAutoLock lock(gIPCLaunchThreadMutex);
  if (!gIPCLaunchThread) {
    nsCOMPtr<nsIThread> thread;
    nsresult rv = NS_NewNamedThread("IPC Launch"_ns, getter_AddRefs(thread));
    if (!NS_WARN_IF(NS_FAILED(rv))) {
      NS_DispatchToMainThread(
          NS_NewRunnableFunction("GeckoChildProcessHost::GetIPCLauncher", [] {
            nsCOMPtr<nsIObserverService> obsService =
                mozilla::services::GetObserverService();
            nsCOMPtr<nsIObserver> obs = new IPCLaunchThreadObserver();
            obsService->AddObserver(obs, "xpcom-shutdown-threads", false);
          }));
      gIPCLaunchThread = thread.forget();
    }
  }

  nsCOMPtr<nsISerialEventTarget> thread = gIPCLaunchThread.get();
  MOZ_DIAGNOSTIC_ASSERT(thread);
  return thread;
}

void
AddAppDirToCommandLine(geckoargs::ChildProcessArgs& aCmdLine,
                       nsIFile* aAppDir, nsIFile* aProfileDir)
{
  if (aAppDir) {
    nsAutoCString path;
    MOZ_ALWAYS_SUCCEEDS(aAppDir->GetNativePath(path));
    geckoargs::sAppDir.Put(path.get(), aCmdLine);

  }
}


RefPtr<ProcessLaunchPromise> BaseProcessLauncher::PerformAsyncLaunch() {
  Result<Ok, LaunchError> aError = DoSetup();
  if (aError.isErr()) {
    return ProcessLaunchPromise::CreateAndReject(aError.unwrapErr(), __func__);
  }

  return DoLaunch()->Then(
      XRE_GetAsyncIOEventTarget(), __func__,
      [self =
           RefPtr{this}](ProcessLaunchPromise::ResolveOrRejectValue&& aResult) {
        self->mChildArgs = {};
        return ProcessLaunchPromise::CreateAndResolveOrReject(
            std::move(aResult), __func__);
      });
}

Result<Ok, LaunchError> BaseProcessLauncher::DoSetup() {
  RefPtr<BaseProcessLauncher> self = this;
#if defined(MOZ_MEMORY)
  if (mProcessType == GeckoProcessType_Content) {
    nsAutoCString mallocOpts(PR_GetEnv("MALLOC_OPTIONS"));
    mallocOpts.Append("r");
    self->mLaunchOptions->env_map[ENVIRONMENT_LITERAL("MALLOC_OPTIONS")] =
        ENVIRONMENT_STRING(mallocOpts.get());
  }
#endif

  MapChildLogging();

  geckoargs::sInitialChannelID.Put(mInitialChannelIdString, mChildArgs);

  geckoargs::sParentPid.Put(static_cast<uint64_t>(base::GetCurrentProcId()),
                            mChildArgs);

  return Ok();
}

void BaseProcessLauncher::MapChildLogging() {
  const char* origNSPRLogName = PR_GetEnv("NSPR_LOG_FILE");
  const char* origMozLogName = PR_GetEnv("MOZ_LOG_FILE");

  if (origNSPRLogName) {
    nsAutoCString nsprLogName;
    GetChildLogName(origNSPRLogName, nsprLogName);
    mLaunchOptions->env_map[ENVIRONMENT_LITERAL("NSPR_LOG_FILE")] =
        ENVIRONMENT_STRING(nsprLogName.get());
  }
  if (origMozLogName) {
    nsAutoCString mozLogName;
    GetChildLogName(origMozLogName, mozLogName);
    mLaunchOptions->env_map[ENVIRONMENT_LITERAL("MOZ_LOG_FILE")] =
        ENVIRONMENT_STRING(mozLogName.get());
  }

  nsAutoCString childRustLog(PR_GetEnv("RUST_LOG_CHILD"));
  if (!childRustLog.IsEmpty()) {
    mLaunchOptions->env_map[ENVIRONMENT_LITERAL("RUST_LOG")] =
        ENVIRONMENT_STRING(childRustLog.get());
  }
}

#if defined(MOZ_WIDGET_GTK)
Result<Ok, LaunchError> LinuxProcessLauncher::DoSetup() {
  Result<Ok, LaunchError> aError = PosixProcessLauncher::DoSetup();
  if (aError.isErr()) {
    return aError;
  }

  if (mProcessType == GeckoProcessType_Content) {
    mLaunchOptions->env_map["GTK_IM_MODULE"] = "gtk-im-context-simple";

    mLaunchOptions->env_map["NO_AT_BRIDGE"] = "1";
  }

  return Ok();
}
#endif

#if defined(XP_UNIX)
Result<Ok, LaunchError> PosixProcessLauncher::DoSetup() {
  Result<Ok, LaunchError> aError = BaseProcessLauncher::DoSetup();
  if (aError.isErr()) {
    return aError;
  }

  if (ShouldHaveDirectoryService()) {
    MOZ_ASSERT(gGREBinPath);
    nsCString path;
    NS_CopyUnicodeToNative(nsDependentString(gGREBinPath), path);
#if defined(XP_LINUX) || 0 || 0 || \
      0 || 0
    const char* ld_library_path = PR_GetEnv("LD_LIBRARY_PATH");
    nsCString new_ld_lib_path(path.get());

    if (ld_library_path && *ld_library_path) {
      new_ld_lib_path.Append(':');
      new_ld_lib_path.Append(ld_library_path);
    }
    mLaunchOptions->env_map["LD_LIBRARY_PATH"] = new_ld_lib_path.get();

#endif
  }

  FilePath exePath;
  BinPathType pathType =
      GetPathToBinary(exePath, mProcessType, mUtilityKind);

  if (pathType == BinPathType::Self) {
    std::string args[]{exePath.value(), "-contentproc"};
    mChildArgs.mArgs.insert(mChildArgs.mArgs.begin(), std::begin(args),
                            std::end(args));
  } else {
    mChildArgs.mArgs.insert(mChildArgs.mArgs.begin(), exePath.value());
  }

  if ((mProcessType == GeckoProcessType_Content ||
       mProcessType == GeckoProcessType_ForkServer) &&
      Omnijar::IsInitialized()) {
    nsAutoCString path;
    nsCOMPtr<nsIFile> greFile = Omnijar::GetPath(Omnijar::GRE);
    if (greFile && NS_SUCCEEDED(greFile->GetNativePath(path))) {
      geckoargs::sGREOmni.Put(path.get(), mChildArgs);
    }
    nsCOMPtr<nsIFile> appFile = Omnijar::GetPath(Omnijar::APP);
    if (appFile && NS_SUCCEEDED(appFile->GetNativePath(path))) {
      geckoargs::sAppOmni.Put(path.get(), mChildArgs);
    }
  }

  AddAppDirToCommandLine(mChildArgs, mAppDir, nullptr);



  mChildArgs.mArgs.push_back(mChildIDString);

  mChildArgs.mArgs.push_back(ChildProcessType());

#if defined(MOZ_ENABLE_FORKSERVER)
  MOZ_ASSERT(mProcessType != GeckoProcessType_ForkServer ||
                 mChildArgs.mFiles.size() == 2,
             "wrong number of FDs for the fork server");
#endif

  geckoargs::AddToFdsToRemap(mChildArgs, mLaunchOptions->fds_to_remap);

  return Ok();
}
#endif


#if defined(XP_UNIX)
RefPtr<ProcessLaunchPromise> PosixProcessLauncher::DoLaunch() {
  Result<Ok, LaunchError> result = Err(LaunchError{"Launch not attempted"});
#if defined(MOZ_ENABLE_FORKSERVER)
  if (mProcessType != GeckoProcessType_ForkServer && ForkServiceChild::Get()) {
    result = ForkServiceChild::Get()->SendForkNewSubprocess(
        std::move(mChildArgs), std::move(*mLaunchOptions), &mResults.mHandle);
  } else
#endif
  {
    result = base::LaunchApp(mChildArgs.mArgs, std::move(*mLaunchOptions),
                             &mResults.mHandle);
  }

  if (result.isErr()) {
    return ProcessLaunchPromise::CreateAndReject(result.unwrapErr(), __func__);
  }
  return ProcessLaunchPromise::CreateAndResolve(std::move(mResults), __func__);
}
#endif




bool GeckoChildProcessHost::OpenPrivilegedHandle(base::ProcessId aPid) {
  if (mChildProcessHandle) {
    MOZ_ASSERT(aPid == base::GetProcId(mChildProcessHandle));
    return true;
  }

  return base::OpenPrivilegedProcessHandle(aPid, &mChildProcessHandle);
}

void GeckoChildProcessHost::OnChannelConnected(base::ProcessId peer_pid) {
  {
    mozilla::AutoWriteLock hLock(mHandleLock);
    if (!OpenPrivilegedHandle(peer_pid)) {
      MOZ_CRASH("can't open handle to child process");
    }
  }
  MonitorAutoLock lock(mMonitor);
  mProcessState = PROCESS_CONNECTED;
  lock.Notify();
}

RefPtr<ProcessHandlePromise> GeckoChildProcessHost::WhenProcessHandleReady() {
  MOZ_ASSERT(mHandlePromise != nullptr);
  return mHandlePromise;
}



void GeckoChildProcessHost::GetAll(const GeckoProcessCallback& aCallback) {
  StaticMutexAutoLock lock(sMutex);
  if (!sGeckoChildProcessHosts) {
    return;
  }
  for (GeckoChildProcessHost* gp = sGeckoChildProcessHosts->getFirst(); gp;
       gp = static_cast<mozilla::LinkedListElement<GeckoChildProcessHost>*>(gp)
                ->getNext()) {
    aCallback(gp);
  }
}

RefPtr<ProcessLaunchPromise> BaseProcessLauncher::Launch(
    GeckoChildProcessHost* aHost) {
  AssertIOThread();

  if (mProcessType != GeckoProcessType_ForkServer) {
    IPC::Channel::ChannelHandle clientHandle;
    if (!aHost->InitializeChannel(&clientHandle)) {
      return ProcessLaunchPromise::CreateAndReject(
          LaunchError("InitializeChannel"), __func__);
    }

    if (auto* handle = std::get_if<UniqueFileHandle>(&clientHandle)) {
      geckoargs::sIPCHandle.Put(std::move(*handle), mChildArgs);
    }
    else {
      MOZ_ASSERT_UNREACHABLE();
      return ProcessLaunchPromise::CreateAndReject(LaunchError("BadPipeType"),
                                                   __func__);
    }
  }

  return InvokeAsync(mLaunchThread, this, __func__,
                     &BaseProcessLauncher::PerformAsyncLaunch);
}

}  
}  
