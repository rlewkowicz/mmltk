/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GMPChild.h"

#include <algorithm>

#include "ChromiumCDMAdapter.h"
#include "base/command_line.h"
#include "base/task.h"
#  include "dlfcn.h"
#include "GMPContentChild.h"
#include "GMPLoader.h"
#include "GMPLog.h"
#include "GMPPlatform.h"
#include "GMPProcessChild.h"
#include "GMPProcessParent.h"
#include "GMPUtils.h"
#include "GMPVideoDecoderChild.h"
#include "GMPVideoEncoderChild.h"
#include "GMPVideoHost.h"
#include "gmp-video-decode.h"
#include "gmp-video-encode.h"
#include "mozilla/TextUtils.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/ProcessChild.h"
#include "nsDebugImpl.h"
#include "nsIFile.h"
#include "nsIXULRuntime.h"
#include "nsReadableUtils.h"
#include "nsThreadManager.h"
#include "nsXPCOM.h"
#include "nsXPCOMPrivate.h"  // for XUL_DLL
#include "nsXULAppAPI.h"
#include "prio.h"
#  include <unistd.h>  // for _exit()

using namespace mozilla::ipc;

namespace mozilla {
namespace gmp {

#define GMP_CHILD_LOG(loglevel, x, ...)                       \
  MOZ_LOG_FMT(GetGMPLog(), (loglevel), "GMPChild[pid={}] " x, \
              (int)base::GetCurrentProcId(), ##__VA_ARGS__)

#define GMP_CHILD_LOG_DEBUG(x, ...) \
  GMP_CHILD_LOG(LogLevel::Debug, x, ##__VA_ARGS__)

GMPChild::GMPChild()
    : mGMPMessageLoop(MessageLoop::current()), mGMPLoader(nullptr) {
  MOZ_ASSERT(NS_IsMainThread());
  GMP_CHILD_LOG_DEBUG("GMPChild ctor");
  nsDebugImpl::SetMultiprocessMode("GMP");
}

GMPChild::~GMPChild() {
  GMP_CHILD_LOG_DEBUG("GMPChild dtor");
  for (auto& libHandle : mLibHandles) {
    dlclose(libHandle);
  }
}

bool GMPChild::Init(const nsAString& aPluginPath, const char* aParentBuildID,
                    mozilla::ipc::UntypedEndpoint&& aEndpoint) {
  GMP_CHILD_LOG_DEBUG("{} pluginPath={} useXpcom={}, useNativeEvent={}",
                      __FUNCTION__, NS_ConvertUTF16toUTF8(aPluginPath).get(),
                      GMPProcessChild::UseXPCOM(),
                      GMPProcessChild::UseNativeEventProcessing());

  if (NS_WARN_IF(NS_FAILED(nsThreadManager::get().Init()))) {
    return false;
  }

  if (NS_WARN_IF(!aEndpoint.Bind(this))) {
    return false;
  }

  MessageChannel* channel = GetIPCChannel();
  if (channel && !channel->SendBuildIDsMatchMessage(aParentBuildID)) {
    ipc::ProcessChild::QuickExit();
  }

  if (GMPProcessChild::UseXPCOM()) {
    if (NS_WARN_IF(NS_FAILED(NS_InitMinimalXPCOM()))) {
      return false;
    }
  }

  mPluginPath = aPluginPath;

  return true;
}

void GMPChild::Shutdown() {
  if (GMPProcessChild::UseXPCOM()) {
    NS_ShutdownXPCOM(nullptr);
  }
}

mozilla::ipc::IPCResult GMPChild::RecvProvideStorageId(
    const nsCString& aStorageId) {
  GMP_CHILD_LOG_DEBUG("{}", __FUNCTION__);
  mStorageId = aStorageId;
  return IPC_OK();
}

GMPErr GMPChild::GetAPI(const char* aAPIName, void* aHostAPI, void** aPluginAPI,
                        const nsACString& aKeySystem) {
  if (!mGMPLoader) {
    return GMPGenericErr;
  }
  return mGMPLoader->GetAPI(aAPIName, aHostAPI, aPluginAPI, aKeySystem);
}

mozilla::ipc::IPCResult GMPChild::RecvPreloadLibs(const nsCString& aLibs) {
  constexpr static const char* whitelist[] = {
      "libfreeblpriv3.so",
      "libsoftokn3.so",
      "libdl.so.2",
      "libpthread.so.0",
      "librt.so.1",
  };

  nsTArray<nsCString> libs;
  SplitAt(", ", aLibs, libs);
  for (const nsCString& lib : libs) {
    for (const char* whiteListedLib : whitelist) {
      if (lib.EqualsASCII(whiteListedLib)) {
        auto libHandle = dlopen(whiteListedLib, RTLD_NOW | RTLD_GLOBAL);
        if (libHandle) {
          mLibHandles.AppendElement(libHandle);
        } else {
          MOZ_CRASH("Couldn't load lib needed by media plugin");
        }
      }
    }
  }
  return IPC_OK();
}

bool GMPChild::GetUTF8LibPath(nsACString& aOutLibPath) {
  nsCOMPtr<nsIFile> libFile;

#  define GMP_PATH_CRASH(explain) \
    do {                          \
      MOZ_CRASH(explain);         \
    } while (false)

  nsresult rv = NS_NewLocalFile(mPluginPath, getter_AddRefs(libFile));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    GMP_PATH_CRASH("Failed to create file for plugin path");
    return false;
  }

  nsCOMPtr<nsIFile> parent;
  rv = libFile->GetParent(getter_AddRefs(parent));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    GMP_PATH_CRASH("Failed to get parent file for plugin file");
    return false;
  }

  nsAutoString parentLeafName;
  rv = parent->GetLeafName(parentLeafName);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    GMP_PATH_CRASH("Failed to get leaf for plugin file");
    return false;
  }

  nsAutoString baseName;
  baseName = Substring(parentLeafName, 4, parentLeafName.Length() - 1);

  nsAutoString binaryName = u"lib"_ns + baseName + u".so"_ns;
  rv = libFile->AppendRelativePath(binaryName);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    GMP_PATH_CRASH("Failed to append lib to plugin file");
    return false;
  }

  if (NS_WARN_IF(!FileExists(libFile))) {
    GMP_PATH_CRASH("Plugin file does not exist");
    return false;
  }

  nsAutoString path;
  rv = libFile->GetPath(path);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    GMP_PATH_CRASH("Failed to get path for plugin file");
    return false;
  }

  CopyUTF16toUTF8(path, aOutLibPath);
  return true;
}

bool GMPChild::GetPluginName(nsACString& aPluginName) const {
  nsCOMPtr<nsIFile> libFile;
  nsresult rv = NS_NewLocalFile(mPluginPath, getter_AddRefs(libFile));
  NS_ENSURE_SUCCESS(rv, false);

  nsCOMPtr<nsIFile> parent;
  rv = libFile->GetParent(getter_AddRefs(parent));
  NS_ENSURE_SUCCESS(rv, false);

  nsAutoString parentLeafName;
  rv = parent->GetLeafName(parentLeafName);
  NS_ENSURE_SUCCESS(rv, false);

  aPluginName.Assign(NS_ConvertUTF16toUTF8(parentLeafName));
  return true;
}

static nsCOMPtr<nsIFile> AppendFile(nsCOMPtr<nsIFile>&& aFile,
                                    const nsString& aStr) {
  return (aFile && NS_SUCCEEDED(aFile->Append(aStr))) ? aFile : nullptr;
}

static nsCOMPtr<nsIFile> CloneFile(const nsCOMPtr<nsIFile>& aFile) {
  nsCOMPtr<nsIFile> clone;
  return (aFile && NS_SUCCEEDED(aFile->Clone(getter_AddRefs(clone)))) ? clone
                                                                      : nullptr;
}

static nsCOMPtr<nsIFile> GetParentFile(const nsCOMPtr<nsIFile>& aFile) {
  nsCOMPtr<nsIFile> parent;
  return (aFile && NS_SUCCEEDED(aFile->GetParent(getter_AddRefs(parent))))
             ? parent
             : nullptr;
}


#  define FIREFOX_FILE MOZ_APP_NAME u""_ns
#define XUL_LIB_FILE XUL_DLL u""_ns

static nsCOMPtr<nsIFile> GetFirefoxAppPath(
    nsCOMPtr<nsIFile> aPluginContainerPath) {
  MOZ_ASSERT(aPluginContainerPath);
  nsCOMPtr<nsIFile> parent = GetParentFile(aPluginContainerPath);
  return parent;
}


static bool AppendHostPath(nsCOMPtr<nsIFile>& aFile,
                           nsTArray<std::pair<nsCString, nsCString>>& aPaths) {
  nsString str;
  if (!FileExists(aFile) || NS_FAILED(aFile->GetPath(str))) {
    return false;
  }

  nsCString filePath = NS_ConvertUTF16toUTF8(str);
  nsCString sigFilePath;
  sigFilePath = nsCString(NS_ConvertUTF16toUTF8(str) + ".sig"_ns);
  aPaths.AppendElement(
      std::make_pair(std::move(filePath), std::move(sigFilePath)));
  return true;
}

nsTArray<std::pair<nsCString, nsCString>>
GMPChild::MakeCDMHostVerificationPaths(const nsACString& aPluginLibPath) {
  nsTArray<std::pair<nsCString, nsCString>> paths;
  paths.AppendElement(
      std::make_pair(nsCString(aPluginLibPath), aPluginLibPath + ".sig"_ns));

  const std::string currentProcessBinary =
      WideToUTF8(CommandLine::ForCurrentProcess()->program());
  nsString str;

  CopyUTF8toUTF16(nsDependentCString(currentProcessBinary.c_str()), str);
  nsCOMPtr<nsIFile> path;
  if (NS_FAILED(NS_NewLocalFile(str, getter_AddRefs(path))) ||
      !AppendHostPath(path, paths)) {
    return paths;
  }

  bool addFirefoxBinaryPath = true;

  nsCOMPtr<nsIFile> appDir = GetFirefoxAppPath(path);
  if (addFirefoxBinaryPath) {
    path = AppendFile(CloneFile(appDir), FIREFOX_FILE);
    if (!AppendHostPath(path, paths)) {
      return paths;
    }
  }

  path = AppendFile(CloneFile(appDir), XUL_LIB_FILE);
  if (!AppendHostPath(path, paths)) {
    return paths;
  }

  return paths;
}

static auto ToCString(const nsTArray<std::pair<nsCString, nsCString>>& aPairs) {
  return StringJoin(","_ns, aPairs, [](nsACString& dest, const auto& p) {
    dest.AppendPrintf("(%s,%s)", p.first.get(), p.second.get());
  });
}

mozilla::ipc::IPCResult GMPChild::RecvStartPlugin(const nsString& aAdapter) {
  GMP_CHILD_LOG_DEBUG("{}", __FUNCTION__);

  nsAutoCString libPath;
  if (!GetUTF8LibPath(libPath)) {
    return IPC_FAIL(this, "Failed to get lib path.");
  }

  auto platformAPI = new GMPPlatformAPI();
  InitPlatformAPI(*platformAPI, this);

  mGMPLoader = MakeUnique<GMPLoader>();

  GMPAdapter* adapter = nullptr;
  if (aAdapter.EqualsLiteral("chromium")) {
    auto&& paths = MakeCDMHostVerificationPaths(libPath);
    GMP_CHILD_LOG_DEBUG("{} CDM host paths={}", __func__,
                        ToCString(paths).get());
    adapter = new ChromiumCDMAdapter(std::move(paths));
  }

  if (!mGMPLoader->Load(libPath.get(), libPath.Length(), platformAPI,
                        adapter)) {
    NS_WARNING("Failed to load GMP");
    delete platformAPI;
    return IPC_FAIL(this, "Failed to load GMP.");
  }

  return IPC_OK();
}

MessageLoop* GMPChild::GMPMessageLoop() { return mGMPMessageLoop; }

void GMPChild::ActorDestroy(ActorDestroyReason aWhy) {
  GMP_CHILD_LOG_DEBUG("{} reason={}", __FUNCTION__, static_cast<int>(aWhy));


  for (uint32_t i = mGMPContentChildren.Length(); i > 0; i--) {
    MOZ_ASSERT_IF(aWhy == NormalShutdown,
                  !mGMPContentChildren[i - 1]->IsUsed());
    mGMPContentChildren[i - 1]->Close();
  }

  if (mGMPLoader) {
    mGMPLoader->Shutdown();
  }

  if (AbnormalShutdown == aWhy) {
    NS_WARNING("Abnormal shutdown of GMP process!");
    ProcessChild::QuickExit();
  }



  XRE_ShutdownChildProcess();
}

void GMPChild::ProcessingError(Result aCode, const char* aReason) {
  switch (aCode) {
    case MsgDropped:
      NS_WARNING("MsgDropped in GMPChild");
      return;
    case MsgNotKnown:
      MOZ_CRASH("aborting because of MsgNotKnown");
    case MsgNotAllowed:
      MOZ_CRASH("aborting because of MsgNotAllowed");
    case MsgPayloadError:
      MOZ_CRASH("aborting because of MsgPayloadError");
    case MsgProcessingError:
      MOZ_CRASH("aborting because of MsgProcessingError");
    case MsgValueError:
      MOZ_CRASH("aborting because of MsgValueError");
    default:
      MOZ_CRASH("not reached");
  }
}

GMPTimerChild* GMPChild::GetGMPTimers() {
  if (auto* timer = SingleManagedOrNull(ManagedPGMPTimerChild())) {
    return static_cast<GMPTimerChild*>(timer);
  }
  auto timerChild = MakeRefPtr<GMPTimerChild>(this);
  if (!SendPGMPTimerConstructor(timerChild)) {
    return nullptr;
  }
  return timerChild.get();
}

GMPStorageChild* GMPChild::GetGMPStorage() {
  if (auto* storage = SingleManagedOrNull(ManagedPGMPStorageChild())) {
    return static_cast<GMPStorageChild*>(storage);
  }
  auto storageChild = MakeRefPtr<GMPStorageChild>(this);
  if (!SendPGMPStorageConstructor(storageChild)) {
    return nullptr;
  }
  return storageChild.get();
}

mozilla::ipc::IPCResult GMPChild::RecvCrashPluginNow() {
  MOZ_CRASH();
  return IPC_OK();
}

mozilla::ipc::IPCResult GMPChild::RecvCloseActive() {
  for (uint32_t i = mGMPContentChildren.Length(); i > 0; i--) {
    mGMPContentChildren[i - 1]->CloseActive();
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult GMPChild::RecvInitGMPContentChild(
    Endpoint<PGMPContentChild>&& aEndpoint) {
  GMPContentChild* child =
      mGMPContentChildren.AppendElement(new GMPContentChild(this))->get();
  aEndpoint.Bind(child);
  return IPC_OK();
}

void GMPChild::GMPContentChildActorDestroy(GMPContentChild* aGMPContentChild) {
  for (uint32_t i = mGMPContentChildren.Length(); i > 0; i--) {
    RefPtr<GMPContentChild>& destroyedActor = mGMPContentChildren[i - 1];
    if (destroyedActor.get() == aGMPContentChild) {
      SendPGMPContentChildDestroyed();
      mGMPContentChildren.RemoveElementAt(i - 1);
      break;
    }
  }
}

mozilla::ipc::IPCResult GMPChild::RecvPreferenceUpdate(const Pref& aPref) {
  Preferences::SetPreference(aPref);
  return IPC_OK();
}

mozilla::ipc::IPCResult GMPChild::RecvShutdown(ShutdownResolver&& aResolver) {
  aResolver(true);
  return IPC_OK();
}


}  
}  

#undef GMP_CHILD_LOG_DEBUG
#undef __CLASS__
