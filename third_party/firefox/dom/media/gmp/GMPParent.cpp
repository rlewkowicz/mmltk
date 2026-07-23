/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GMPParent.h"

#include "CDMStorageIdProvider.h"
#include "ChromiumCDMAdapter.h"
#include "GMPContentParent.h"
#include "GMPLog.h"
#include "GMPTimerParent.h"
#include "MediaResult.h"
#include "mozIGeckoMediaPluginService.h"
#include "mozilla/Casting.h"
#include "mozilla/dom/KeySystemNames.h"
#include "mozilla/dom/WidevineCDMManifestBinding.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/GeckoChildProcessHost.h"
#include "mozilla/SSE.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/SyncRunnable.h"
#include "nsComponentManagerUtils.h"
#include "nsIObserverService.h"
#include "nsIRunnable.h"
#include "nsPrintfCString.h"
#include "nsThreadUtils.h"
#include "runnable_utils.h"

using mozilla::ipc::GeckoChildProcessHost;

namespace mozilla::gmp {

#define GMP_PARENT_LOG_DEBUG(x, ...)                                       \
  GMP_LOG_DEBUG("GMPParent[{}|childPid={}] " x, fmt::ptr(this), mChildPid, \
                ##__VA_ARGS__)

#ifdef __CLASS__
#  undef __CLASS__
#endif
#define __CLASS__ "GMPParent"

GMPParent::GMPParent()
    : mState(GMPState::NotLoaded),
      mPluginId(GeckoChildProcessHost::GetUniqueID()),
      mProcess(nullptr),
      mDeleteProcessOnlyOnUnload(false),
      mAbnormalShutdownInProgress(false),
      mIsBlockingDeletion(false),
      mCanDecrypt(false),
      mGMPContentChildCount(0),
      mChildPid(0),
#ifdef ALLOW_GECKO_CHILD_PROCESS_ARCH
      mChildLaunchArch(base::PROCESS_ARCH_INVALID),
#endif
      mMainThread(GetMainThreadSerialEventTarget()) {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  GMP_PARENT_LOG_DEBUG("GMPParent ctor id={}", mPluginId);
}

GMPParent::~GMPParent() {
  GMP_PARENT_LOG_DEBUG("GMPParent dtor id={}", mPluginId);
  MOZ_ASSERT(!mProcess);
}

void GMPParent::CloneFrom(const GMPParent* aOther) {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  MOZ_ASSERT(aOther->mService);

  mService = aOther->mService;
  mDirectory = aOther->mDirectory;
  mName = aOther->mName;
  mVersion = aOther->mVersion;
  mDescription = aOther->mDescription;
  mDisplayName = aOther->mDisplayName;
  mPluginType = aOther->mPluginType;
  mLibs = aOther->mLibs;
  for (const GMPCapability& cap : aOther->mCapabilities) {
    mCapabilities.AppendElement(cap);
  }
  mAdapter = aOther->mAdapter;

#ifdef ALLOW_GECKO_CHILD_PROCESS_ARCH
  mChildLaunchArch = aOther->mChildLaunchArch;
#endif
}



RefPtr<GenericPromise> GMPParent::Init(GeckoMediaPluginServiceParent* aService,
                                       nsIFile* aPluginDir) {
  MOZ_ASSERT(aPluginDir);
  MOZ_ASSERT(aService);
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());

  mService = aService;
  mDirectory = aPluginDir;

  nsCOMPtr<nsIFile> parent;
  nsresult rv = aPluginDir->GetParent(getter_AddRefs(parent));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return GenericPromise::CreateAndReject(rv, __func__);
  }
  nsAutoString parentLeafName;
  rv = parent->GetLeafName(parentLeafName);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return GenericPromise::CreateAndReject(rv, __func__);
  }
  GMP_PARENT_LOG_DEBUG("{}: for {}", __FUNCTION__,
                       NS_LossyConvertUTF16toASCII(parentLeafName).get());

  MOZ_ASSERT(parentLeafName.Length() > 4);
  mName = Substring(parentLeafName, 4);


  return ReadGMPMetaData();
}

void GMPParent::Crash() {
  if (mState != GMPState::NotLoaded) {
    (void)SendCrashPluginNow();
  }
}

nsresult GMPParent::LoadProcess() {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  MOZ_ASSERT(mState == GMPState::NotLoaded);

  if (NS_WARN_IF(mPluginType == GMPPluginType::WidevineL1)) {
    GMP_PARENT_LOG_DEBUG("{}: cannot load process for WidevineL1",
                         __FUNCTION__);
    return NS_ERROR_NOT_IMPLEMENTED;
  }

  nsAutoString path;
  if (NS_WARN_IF(!mDirectory) ||
      NS_WARN_IF(NS_FAILED(mDirectory->GetPath(path)))) {
    return NS_ERROR_FAILURE;
  }
  GMP_PARENT_LOG_DEBUG("{}: for {}", __FUNCTION__,
                       NS_ConvertUTF16toUTF8(path).get());

  if (!mProcess) {
    mProcess = new GMPProcessParent(NS_ConvertUTF16toUTF8(path).get());

#ifdef ALLOW_GECKO_CHILD_PROCESS_ARCH
    mProcess->SetLaunchArchitecture(mChildLaunchArch);
#endif

#if defined(MOZ_CODE_COVERAGE)
    constexpr int32_t kLaunchTimeoutMs = 60 * 1000;
#else
    constexpr int32_t kLaunchTimeoutMs = 30 * 1000;
#endif
    if (!mProcess->Launch(kLaunchTimeoutMs)) {
      GMP_PARENT_LOG_DEBUG("{}: Failed to launch new child process",
                           __FUNCTION__);
      mProcess->Delete();
      mProcess = nullptr;
      return NS_ERROR_FAILURE;
    }

    mChildPid = mProcess->GetChildProcessId();
    GMP_PARENT_LOG_DEBUG("{}: Launched new child process", __FUNCTION__);

    bool opened = mProcess->TakeInitialEndpoint().Bind(this);
    if (!opened) {
      GMP_PARENT_LOG_DEBUG("{}: Failed to open channel to new child process",
                           __FUNCTION__);
      mProcess->Delete();
      mProcess = nullptr;
      return NS_ERROR_FAILURE;
    }
    GMP_PARENT_LOG_DEBUG("{}: Opened channel to new child process",
                         __FUNCTION__);

    bool ok =
        SendProvideStorageId(CDMStorageIdProvider::ComputeStorageId(mNodeId));
    if (!ok) {
      GMP_PARENT_LOG_DEBUG("{}: Failed to send storage id to child process",
                           __FUNCTION__);
      return NS_ERROR_FAILURE;
    }
    GMP_PARENT_LOG_DEBUG("{}: Sent storage id to child process", __FUNCTION__);

    if (!mLibs.IsEmpty()) {
      bool ok = SendPreloadLibs(mLibs);
      if (!ok) {
        GMP_PARENT_LOG_DEBUG("{}: Failed to send preload-libs to child process",
                             __FUNCTION__);
        return NS_ERROR_FAILURE;
      }
      GMP_PARENT_LOG_DEBUG("{}: Sent preload-libs ('{}') to child process",
                           __FUNCTION__, mLibs.get());
    }

    if (!SendStartPlugin(mAdapter)) {
      GMP_PARENT_LOG_DEBUG("{}: Failed to send start to child process",
                           __FUNCTION__);
      return NS_ERROR_FAILURE;
    }
    GMP_PARENT_LOG_DEBUG("{}: Sent StartPlugin to child process", __FUNCTION__);
  }

  mState = GMPState::Loaded;

  return NS_OK;
}

void GMPParent::OnPreferenceChange(const mozilla::dom::Pref& aPref) {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  GMP_PARENT_LOG_DEBUG("{}", __FUNCTION__);

  if (!mProcess || !mProcess->UseXPCOM()) {
    return;
  }

  (void)SendPreferenceUpdate(aPref);
}

mozilla::ipc::IPCResult GMPParent::RecvPGMPContentChildDestroyed() {
  --mGMPContentChildCount;
  if (!IsUsed()) {
    CloseIfUnused();
  }
  return IPC_OK();
}

void GMPParent::CloseIfUnused() {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  GMP_PARENT_LOG_DEBUG("{}", __FUNCTION__);

  if ((mDeleteProcessOnlyOnUnload || mState == GMPState::Loaded ||
       mState == GMPState::Unloading) &&
      !IsUsed()) {
    for (auto* timer : ManagedPGMPTimerParent()) {
      static_cast<GMPTimerParent*>(timer)->Shutdown();
    }

    for (auto* storage : ManagedPGMPStorageParent()) {
      static_cast<GMPStorageParent*>(storage)->Shutdown();
    }
    Shutdown();
  }
}

void GMPParent::CloseActive(bool aDieWhenUnloaded) {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  GMP_PARENT_LOG_DEBUG("{}: state {}", __FUNCTION__,
                       uint32_t(GMPState(mState)));

  if (aDieWhenUnloaded) {
    mDeleteProcessOnlyOnUnload = true;  
  }
  if (mState == GMPState::Loaded) {
    mState = GMPState::Unloading;
  }
  if (mState != GMPState::NotLoaded && IsUsed()) {
    (void)SendCloseActive();
    CloseIfUnused();
  }
}

void GMPParent::MarkForDeletion() {
  mDeleteProcessOnlyOnUnload = true;
  mIsBlockingDeletion = true;
}

bool GMPParent::IsMarkedForDeletion() { return mIsBlockingDeletion; }

void GMPParent::Shutdown() {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  GMP_PARENT_LOG_DEBUG("{}", __FUNCTION__);

  if (mAbnormalShutdownInProgress) {
    return;
  }

  MOZ_ASSERT(!IsUsed());
  switch (mState) {
    case GMPState::NotLoaded:
    case GMPState::Closing:
    case GMPState::Closed:
      return;
    default:
      break;
  }

  RefPtr<GMPParent> self(this);
  DeleteProcess();

  if (!mDeleteProcessOnlyOnUnload) {
    mService->ReAddOnGMPThread(self);
  }  
  MOZ_ASSERT(mState == GMPState::NotLoaded || mState == GMPState::Closing);
}

class NotifyGMPShutdownTask : public Runnable {
 public:
  explicit NotifyGMPShutdownTask(const nsAString& aNodeId)
      : Runnable("NotifyGMPShutdownTask"), mNodeId(aNodeId) {}
  NS_IMETHOD Run() override {
    MOZ_ASSERT(NS_IsMainThread());
    nsCOMPtr<nsIObserverService> obsService =
        mozilla::services::GetObserverService();
    MOZ_ASSERT(obsService);
    if (obsService) {
      obsService->NotifyObservers(nullptr, "gmp-shutdown", mNodeId.get());
    }
    return NS_OK;
  }
  nsString mNodeId;
};

void GMPParent::ChildTerminated() {
  RefPtr<GMPParent> self(this);
  nsCOMPtr<nsISerialEventTarget> gmpEventTarget = GMPEventTarget();

  if (!gmpEventTarget) {
    GMP_PARENT_LOG_DEBUG("{}::{}: GMPEventTarget() returned nullptr.",
                         __CLASS__, __FUNCTION__);
  } else {
    gmpEventTarget->Dispatch(
        NewRunnableMethod<RefPtr<GMPParent>>(
            "gmp::GeckoMediaPluginServiceParent::PluginTerminated", mService,
            &GeckoMediaPluginServiceParent::PluginTerminated, self),
        NS_DISPATCH_NORMAL);
  }
}

void GMPParent::DeleteProcess() {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());

  switch (mState) {
    case GMPState::Closed:
      break;
    case GMPState::Closing:
      GMP_PARENT_LOG_DEBUG("{}: Shutdown handshake in progress.", __FUNCTION__);
      return;
    default: {
      GMP_PARENT_LOG_DEBUG("{}: Shutdown handshake starting.", __FUNCTION__);

      RefPtr<GMPParent> self = this;
      nsCOMPtr<nsISerialEventTarget> gmpEventTarget = GMPEventTarget();
      mState = GMPState::Closing;
      SendShutdown()->Then(
          gmpEventTarget, __func__,
          [self](bool) {
            GMP_LOG_DEBUG(
                "GMPParent[{}|childPid={}] DeleteProcess: Shutdown handshake "
                "success.",
                fmt::ptr(self.get()), self->mChildPid);
            self->mState = GMPState::Closed;
            self->Close();
            self->DeleteProcess();
          },
          [self](const ipc::ResponseRejectReason&) {
            GMP_LOG_DEBUG(
                "GMPParent[{}|childPid={}] DeleteProcess: Shutdown handshake "
                "error.",
                fmt::ptr(self.get()), self->mChildPid);
          });
      return;
    }
  }

  GMP_PARENT_LOG_DEBUG("{}: Shutting down process.", __FUNCTION__);
  mProcess->Delete(NewRunnableMethod("gmp::GMPParent::ChildTerminated", this,
                                     &GMPParent::ChildTerminated));
  GMP_PARENT_LOG_DEBUG("{}: Shut down process", __FUNCTION__);
  mProcess = nullptr;


  mState = GMPState::NotLoaded;

  nsCOMPtr<nsIRunnable> r =
      new NotifyGMPShutdownTask(NS_ConvertUTF8toUTF16(mNodeId));
  mMainThread->Dispatch(r.forget());
}

GMPState GMPParent::State() const { return mState; }

nsCOMPtr<nsISerialEventTarget> GMPParent::GMPEventTarget() {
  nsCOMPtr<mozIGeckoMediaPluginService> mps =
      do_GetService("@mozilla.org/gecko-media-plugin-service;1");
  MOZ_ASSERT(mps);
  if (!mps) {
    return nullptr;
  }
  nsCOMPtr<nsIThread> gmpThread;
  mps->GetThread(getter_AddRefs(gmpThread));
  return gmpThread;
}

bool GMPCapability::Supports(const nsTArray<GMPCapability>& aCapabilities,
                             const nsACString& aAPI,
                             const nsTArray<nsCString>& aTags) {
  for (const nsCString& tag : aTags) {
    if (!GMPCapability::Supports(aCapabilities, aAPI, tag)) {
      return false;
    }
  }
  return true;
}

bool GMPCapability::Supports(const nsTArray<GMPCapability>& aCapabilities,
                             const nsACString& aAPI, const nsCString& aTag) {
  for (const GMPCapability& capabilities : aCapabilities) {
    if (!capabilities.mAPIName.Equals(aAPI)) {
      continue;
    }
    for (const nsCString& tag : capabilities.mAPITags) {
      if (tag.Equals(aTag)) {
        return true;
      }
    }
  }
  return false;
}

bool GMPParent::EnsureProcessLoaded() {
  switch (mState) {
    case GMPState::NotLoaded:
      return NS_SUCCEEDED(LoadProcess());
    case GMPState::Loaded:
      return true;
    case GMPState::Unloading:
    case GMPState::Closing:
    case GMPState::Closed:
      return false;
  }

  MOZ_ASSERT_UNREACHABLE("Unhandled GMPState!");
  return false;
}

void GMPParent::ActorDestroy(ActorDestroyReason aWhy) {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  GMP_PARENT_LOG_DEBUG("{}: ({}), state={}", __FUNCTION__, (int)aWhy,
                       uint32_t(GMPState(mState)));

  mState = GMPState::Closed;
  mAbnormalShutdownInProgress = true;
  CloseActive(false);

  if (AbnormalShutdown == aWhy) {
    RefPtr<GMPParent> self(this);
    MOZ_ASSERT(mState == GMPState::Closed);
    DeleteProcess();
    mService->ReAddOnGMPThread(self);
  }
}

already_AddRefed<PGMPStorageParent> GMPParent::AllocPGMPStorageParent() {
  auto p = MakeRefPtr<GMPStorageParent>(mNodeId, this);
  if (NS_WARN_IF(NS_FAILED(p->Init()))) {
    return nullptr;
  }
  return p.forget();
}

already_AddRefed<PGMPTimerParent> GMPParent::AllocPGMPTimerParent() {
  nsCOMPtr<nsISerialEventTarget> target = GMPEventTarget();
  return MakeAndAddRef<GMPTimerParent>(std::move(target));
}

bool ReadInfoField(GMPInfoFileParser& aParser, const nsCString& aKey,
                   nsACString& aOutValue) {
  if (!aParser.Contains(aKey) || aParser.Get(aKey).IsEmpty()) {
    return false;
  }
  aOutValue = aParser.Get(aKey);
  return true;
}

RefPtr<GenericPromise> GMPParent::ReadGMPMetaData() {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  MOZ_ASSERT(mDirectory, "Plugin directory cannot be NULL!");
  MOZ_ASSERT(!mName.IsEmpty(), "Plugin mName cannot be empty!");

  nsCOMPtr<nsIFile> infoFile;
  nsresult rv = mDirectory->Clone(getter_AddRefs(infoFile));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return GenericPromise::CreateAndReject(rv, __func__);
  }
  infoFile->AppendRelativePath(mName + u".info"_ns);

  if (FileExists(infoFile)) {
    return ReadGMPInfoFile(infoFile);
  }

  nsCOMPtr<nsIFile> manifestFile;
  rv = mDirectory->Clone(getter_AddRefs(manifestFile));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return GenericPromise::CreateAndReject(rv, __func__);
  }
  manifestFile->AppendRelativePath(u"manifest.json"_ns);
  return ReadChromiumManifestFile(manifestFile);
}

static void ApplyGlibcWorkaround(nsCString& aLibs) {
  if (!aLibs.IsEmpty()) {
    aLibs.AppendLiteral(", ");
  }
  aLibs.AppendLiteral("libdl.so.2, libpthread.so.0, librt.so.1");
}


static constexpr uint64_t MakeVersion(uint16_t aA, uint16_t aB, uint16_t aC,
                                      uint16_t aD) {
  return (static_cast<uint64_t>(aA) << 48) | (static_cast<uint64_t>(aB) << 32) |
         (static_cast<uint64_t>(aC) << 16) | aD;
}

static nsresult ParseVersion(const nsACString& aVersion,
                             uint64_t* aParsedVersion) {
  MOZ_ASSERT(aParsedVersion);

  uint64_t version = 0;
  uint32_t fragmentCount = 0;
  nsresult rv = NS_OK;

  for (const auto& fragment : aVersion.Split('.')) {
    ++fragmentCount;
    if (NS_WARN_IF(fragmentCount >= 5)) {
      return NS_ERROR_FAILURE;
    }

    uint32_t fragmentInt = fragment.ToUnsignedInteger(&rv,  10);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    version = (version << 16) | SaturatingCast<uint16_t>(fragmentInt);
  }

  *aParsedVersion = version << (4 - fragmentCount) * 16;
  return NS_OK;
}

RefPtr<GenericPromise> GMPParent::ReadGMPInfoFile(nsIFile* aFile) {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  GMPInfoFileParser parser;
  if (!parser.Init(aFile)) {
    return GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  nsAutoCString apis;
  if (!ReadInfoField(parser, "name"_ns, mDisplayName) ||
      !ReadInfoField(parser, "description"_ns, mDescription) ||
      !ReadInfoField(parser, "version"_ns, mVersion) ||
      !ReadInfoField(parser, "apis"_ns, apis)) {
    return GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  ReadInfoField(parser, "libraries"_ns, mLibs);

  UpdatePluginType();

  bool addMozSupportsH264Advanced = false;
  bool addMozSupportsH264TemporalSVC = false;
  if (mPluginType == GMPPluginType::OpenH264) {
    uint64_t parsedVersion = 0;
    nsresult rv = ParseVersion(mVersion, &parsedVersion);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return GenericPromise::CreateAndReject(rv, __func__);
    }

    addMozSupportsH264Advanced = parsedVersion >= MakeVersion(2, 3, 2, 0);

    addMozSupportsH264TemporalSVC = parsedVersion > MakeVersion(2, 5, 0, 0);
  }

  if (mPluginType != GMPPluginType::Clearkey) {
    ApplyGlibcWorkaround(mLibs);
  }


  nsTArray<nsCString> apiTokens;
  SplitAt(", ", apis, apiTokens);
  for (const nsCString& api : apiTokens) {
    int32_t tagsStart = api.FindChar('[');
    if (tagsStart == 0) {
      continue;
    }

    GMPCapability cap;
    if (tagsStart == -1) {
      cap.mAPIName.Assign(api);
    } else {
      auto tagsEnd = api.FindChar(']');
      if (tagsEnd == -1 || tagsEnd < tagsStart) {
        continue;
      }

      cap.mAPIName.Assign(Substring(api, 0, tagsStart));

      if ((tagsEnd - tagsStart) > 1) {
        const nsDependentCSubstring ts(
            Substring(api, tagsStart + 1, tagsEnd - tagsStart - 1));
        nsTArray<nsCString> tagTokens;
        SplitAt(":", ts, tagTokens);
        for (const nsCString& tag : tagTokens) {
          cap.mAPITags.AppendElement(tag);
        }
      }
    }

    if (mPluginType == GMPPluginType::OpenH264) {
      if (addMozSupportsH264Advanced &&
          !cap.mAPITags.Contains("moz-h264-advanced"_ns)) {
        cap.mAPITags.AppendElement("moz-h264-advanced"_ns);
      }
      if (addMozSupportsH264TemporalSVC &&
          !cap.mAPITags.Contains("moz-h264-temporal-svc"_ns)) {
        cap.mAPITags.AppendElement("moz-h264-temporal-svc"_ns);
      }
    }

    mCapabilities.AppendElement(std::move(cap));
  }

  if (mCapabilities.IsEmpty()) {
    return GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  return GenericPromise::CreateAndResolve(true, __func__);
}

RefPtr<GenericPromise> GMPParent::ReadChromiumManifestFile(nsIFile* aFile) {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  nsAutoCString json;
  if (!ReadIntoString(aFile, json, 5 * 1024)) {
    return GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  return InvokeAsync(mMainThread, this, __func__,
                     &GMPParent::ParseChromiumManifest,
                     NS_ConvertUTF8toUTF16(json));
}

static bool IsCDMAPISupported(
    const mozilla::dom::WidevineCDMManifest& aManifest) {
  if (!aManifest.mX_cdm_module_versions.WasPassed() ||
      !aManifest.mX_cdm_interface_versions.WasPassed() ||
      !aManifest.mX_cdm_host_versions.WasPassed()) {
    return false;
  }

  nsresult ignored;  
  int32_t moduleVersion =
      aManifest.mX_cdm_module_versions.Value().ToInteger(&ignored);
  int32_t interfaceVersion =
      aManifest.mX_cdm_interface_versions.Value().ToInteger(&ignored);
  int32_t hostVersion =
      aManifest.mX_cdm_host_versions.Value().ToInteger(&ignored);
  return ChromiumCDMAdapter::Supports(moduleVersion, interfaceVersion,
                                      hostVersion);
}

RefPtr<GenericPromise> GMPParent::ParseChromiumManifest(
    const nsAString& aJSON) {
  GMP_PARENT_LOG_DEBUG("{}: for '{}'", __FUNCTION__,
                       NS_LossyConvertUTF16toASCII(aJSON).get());

  MOZ_ASSERT(NS_IsMainThread());
  mozilla::dom::WidevineCDMManifest m;
  if (!m.Init(aJSON)) {
    GMP_PARENT_LOG_DEBUG("{}: Failed to initialize json parser, failing.",
                         __FUNCTION__);
    return GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  CopyUTF16toUTF8(m.mName, mDisplayName);
  CopyUTF16toUTF8(m.mVersion, mVersion);

  if (m.mDescription.WasPassed()) {
    CopyUTF16toUTF8(m.mDescription.Value(), mDescription);
  }


  UpdatePluginType();

  GMPCapability video;

  if (IsCDMAPISupported(m)) {
    video.mAPIName = nsLiteralCString(CHROMIUM_CDM_API);
    mAdapter = u"chromium"_ns;
#ifdef MOZ_WMF_CDM
  } else if (mPluginType == GMPPluginType::WidevineL1) {
    video.mAPIName = nsCString(kWidevineExperimentAPIName);
    mAdapter = NS_ConvertUTF8toUTF16(kWidevineExperimentAPIName);
#endif
  } else {
    GMP_PARENT_LOG_DEBUG("{}: CDM API not supported, failing.", __FUNCTION__);
    return GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  switch (mPluginType) {
    case GMPPluginType::Clearkey:
      video.mAPITags.AppendElement(nsCString{kClearKeyKeySystemName});
      mLibs = "libfreeblpriv3.so, libsoftokn3.so"_ns;
      break;
    case GMPPluginType::Widevine:
      video.mAPITags.AppendElement(nsCString{kWidevineKeySystemName});
      break;
#ifdef MOZ_WMF_CDM
    case GMPPluginType::WidevineL1:
      video.mAPITags.AppendElement(nsCString{kWidevineExperimentKeySystemName});
      video.mAPITags.AppendElement(
          nsCString{kWidevineExperiment2KeySystemName});
      break;
#endif
    case GMPPluginType::Fake:
      video.mAPITags.AppendElement(nsCString{"fake"});
      break;
    default:
      GMP_PARENT_LOG_DEBUG("{}: Unrecognized key system: {}, failing.",
                           __FUNCTION__, mDisplayName.get());
      return GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  ApplyGlibcWorkaround(mLibs);


  nsTArray<nsCString> codecs;

  if (m.mX_cdm_codecs.WasPassed()) {
    nsCString codecsString;
    codecsString = NS_ConvertUTF16toUTF8(m.mX_cdm_codecs.Value());
    SplitAt(",", codecsString, codecs);
  }

  for (const nsCString& chromiumCodec : codecs) {
    nsCString codec;
    if (chromiumCodec.EqualsASCII("vp8")) {
      codec = "vp8"_ns;
    } else if (chromiumCodec.EqualsASCII("vp9.0") ||  
               chromiumCodec.EqualsASCII("vp09")) {
      codec = "vp9"_ns;
    } else if (chromiumCodec.EqualsASCII("avc1")) {
      codec = "h264"_ns;
    } else if (chromiumCodec.EqualsASCII("av01")) {
      codec = "av1"_ns;
    } else {
      GMP_PARENT_LOG_DEBUG("{}: Unrecognized codec: {}.", __FUNCTION__,
                           chromiumCodec.get());
      MOZ_ASSERT_UNREACHABLE(
          "Unhandled codec string! Need to add it to the parser.");
      continue;
    }

    video.mAPITags.AppendElement(codec);
  }

  mCapabilities.AppendElement(std::move(video));

  GMP_PARENT_LOG_DEBUG("{}: Successfully parsed manifest.", __FUNCTION__);
  return GenericPromise::CreateAndResolve(true, __func__);
}

bool GMPParent::CanBeSharedCrossNodeIds() const {
  return mNodeId.IsEmpty() &&
         !mCanDecrypt;
}

bool GMPParent::CanBeUsedFrom(const nsACString& aNodeId) const {
  return mNodeId == aNodeId;
}

void GMPParent::SetNodeId(const nsACString& aNodeId) {
  MOZ_ASSERT(!aNodeId.IsEmpty());
  mNodeId = aNodeId;
}

void GMPParent::UpdatePluginType() {
  if (mDisplayName.EqualsLiteral("WidevineCdm")) {
    mPluginType = GMPPluginType::Widevine;
#ifdef MOZ_WMF_CDM
  } else if (mDisplayName.EqualsLiteral(kWidevineExperimentAPIName)) {
    mPluginType = GMPPluginType::WidevineL1;
#endif
  } else if (mDisplayName.EqualsLiteral("gmpopenh264")) {
    mPluginType = GMPPluginType::OpenH264;
  } else if (mDisplayName.EqualsLiteral("clearkey")) {
    mPluginType = GMPPluginType::Clearkey;
  } else if (mDisplayName.EqualsLiteral("fake")) {
    mPluginType = GMPPluginType::Fake;
  } else {
    mPluginType = GMPPluginType::Unknown;
  }
}

const nsCString& GMPParent::GetDisplayName() const { return mDisplayName; }

const nsCString& GMPParent::GetVersion() const { return mVersion; }

uint32_t GMPParent::GetPluginId() const { return mPluginId; }

void GMPParent::ResolveGetContentParentPromises() {
  nsTArray<UniquePtr<MozPromiseHolder<GetGMPContentParentPromise>>> promises =
      std::move(mGetContentParentPromises);
  MOZ_ASSERT(mGetContentParentPromises.IsEmpty());
  RefPtr<GMPContentParentCloseBlocker> blocker(
      new GMPContentParentCloseBlocker(mGMPContentParent));
  for (auto& holder : promises) {
    holder->Resolve(blocker, __func__);
  }
}

bool GMPParent::OpenPGMPContent() {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  MOZ_ASSERT(!mGMPContentParent);

  Endpoint<PGMPContentParent> parent;
  Endpoint<PGMPContentChild> child;
  if (NS_WARN_IF(NS_FAILED(PGMPContent::CreateEndpoints(
          mozilla::ipc::EndpointProcInfo::Current(), OtherEndpointProcInfo(),
          &parent, &child)))) {
    return false;
  }

  mGMPContentParent = new GMPContentParent(this);

  if (!parent.Bind(mGMPContentParent)) {
    return false;
  }

  if (!SendInitGMPContentChild(std::move(child))) {
    return false;
  }

  ResolveGetContentParentPromises();

  return true;
}

void GMPParent::RejectGetContentParentPromises() {
  nsTArray<UniquePtr<MozPromiseHolder<GetGMPContentParentPromise>>> promises =
      std::move(mGetContentParentPromises);
  MOZ_ASSERT(mGetContentParentPromises.IsEmpty());
  for (auto& holder : promises) {
    holder->Reject(NS_ERROR_FAILURE, __func__);
  }
}

void GMPParent::GetGMPContentParent(
    UniquePtr<MozPromiseHolder<GetGMPContentParentPromise>>&& aPromiseHolder) {
  MOZ_ASSERT(GMPEventTarget()->IsOnCurrentThread());
  GMP_PARENT_LOG_DEBUG("{} {}", __FUNCTION__, fmt::ptr(this));

  if (mGMPContentParent) {
    RefPtr<GMPContentParentCloseBlocker> blocker(
        new GMPContentParentCloseBlocker(mGMPContentParent));
    aPromiseHolder->Resolve(blocker, __func__);
  } else {
    mGetContentParentPromises.AppendElement(std::move(aPromiseHolder));
    if (mGetContentParentPromises.Length() == 1) {
      if (!EnsureProcessLoaded() || !OpenPGMPContent()) {
        RejectGetContentParentPromises();
        return;
      }
      ++mGMPContentChildCount;
    }
  }
}

already_AddRefed<GMPContentParent> GMPParent::ForgetGMPContentParent() {
  MOZ_ASSERT(mGetContentParentPromises.IsEmpty());
  return mGMPContentParent.forget();
}

bool GMPParent::EnsureProcessLoaded(base::ProcessId* aID) {
  if (!EnsureProcessLoaded()) {
    return false;
  }
  *aID = OtherPid();
  return true;
}

void GMPParent::IncrementGMPContentChildCount() { ++mGMPContentChildCount; }

nsString GMPParent::GetPluginBaseName() const { return u"gmp-"_ns + mName; }


}  

#undef GMP_PARENT_LOG_DEBUG
#undef __CLASS__
