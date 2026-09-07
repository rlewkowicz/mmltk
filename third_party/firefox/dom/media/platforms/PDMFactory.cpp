/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PDMFactory.h"

#include "AOMDecoder.h"
#include "AgnosticDecoderModule.h"
#include "AudioTrimmer.h"
#include "BlankDecoderModule.h"
#include "DecoderDoctorDiagnostics.h"
#include "H264.h"
#include "MP4Decoder.h"
#include "MediaChangeMonitor.h"
#include "MediaInfo.h"
#include "PDMFactorySupport.h"
#include "VPXDecoder.h"
#include "VideoUtils.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/RemoteDecodeUtils.h"
#include "mozilla/RemoteDecoderModule.h"
#include "mozilla/RemoteMediaManagerChild.h"
#include "mozilla/SharedThreadPool.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/SyncRunnable.h"
#include "mozilla/TaskQueue.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/ipc/UtilityMediaServiceParent.h"
#include "nsIXULRuntime.h"  // for BrowserTabsRemoteAutostart
#include "nsPrintfCString.h"

#if defined(MOZ_FFMPEG)
#  include "FFmpegRuntimeLinker.h"
#endif
#if defined(MOZ_OMX)
#  include "OmxDecoderModule.h"
#endif
#include <functional>

#include "FFVPXRuntimeLinker.h"

using DecodeSupport = mozilla::media::DecodeSupport;
using DecodeSupportSet = mozilla::media::DecodeSupportSet;
using MediaCodec = mozilla::media::MediaCodec;
using MediaCodecsSupport = mozilla::media::MediaCodecsSupport;
using MediaCodecsSupported = mozilla::media::MediaCodecsSupported;
using MCSInfo = mozilla::media::MCSInfo;

namespace mozilla {

#define PDM_INIT_LOG(msg, ...) \
  MOZ_LOG_FMT(sPDMLog, LogLevel::Debug, "PDMInitializer, " msg, ##__VA_ARGS__)

extern already_AddRefed<PlatformDecoderModule> CreateNullDecoderModule();

constinit static StaticDataMutex<StaticRefPtr<PlatformDecoderModule>>
    sForcedPDM("Forced PDM");

class PDMInitializer final {
 public:
  static void InitPDMs();

  static bool HasInitializedPDMs();

 private:
  static void InitGpuPDMs() {
  }

  static void InitRddPDMs() {
#if defined(MOZ_FFMPEG)
    if (StaticPrefs::media_rdd_ffmpeg_enabled()) {
      FFmpegRuntimeLinker::Init();
    }
#endif
    FFVPXRuntimeLinker::Init();
  }

  static void InitUtilityPDMs() {
    const ipc::UtilityProcessKind kind = GetCurrentUtilityProcessKind();
    if (kind == ipc::UtilityProcessKind::GENERIC_UTILITY) {
      FFVPXRuntimeLinker::Init();
    }
#if defined(MOZ_FFMPEG)
    if (kind == ipc::UtilityProcessKind::GENERIC_UTILITY) {
      FFmpegRuntimeLinker::Init();
    }
#endif
  }

  static void InitContentPDMs() {
#if !0  // Still required for video?
    if (StaticPrefs::media_allow_audio_non_utility()) {
#endif
#if defined(MOZ_OMX)
      OmxDecoderModule::Init();
#endif
      FFVPXRuntimeLinker::Init();
#if defined(MOZ_FFMPEG)
      FFmpegRuntimeLinker::Init();
#endif
#if !0  // Still required for video?
    }
#endif

    RemoteMediaManagerChild::Init();
  }

  static void InitDefaultPDMs() {
#if defined(MOZ_OMX)
    OmxDecoderModule::Init();
#endif
    FFVPXRuntimeLinker::Init();
#if defined(MOZ_FFMPEG)
    FFmpegRuntimeLinker::Init();
#endif
  }

  static bool sHasInitializedPDMs;
  static StaticMutex sMonitor MOZ_UNANNOTATED;
};

bool PDMInitializer::sHasInitializedPDMs = false;
StaticMutex PDMInitializer::sMonitor;

void PDMInitializer::InitPDMs() {
  StaticMutexAutoLock mon(sMonitor);
  if (sHasInitializedPDMs) {
    return;
  }
  if (XRE_IsGPUProcess()) {
    PDM_INIT_LOG("Init PDMs in GPU process");
    InitGpuPDMs();
  } else if (XRE_IsRDDProcess()) {
    PDM_INIT_LOG("Init PDMs in RDD process");
    InitRddPDMs();
  } else if (XRE_IsUtilityProcess()) {
    PDM_INIT_LOG("Init PDMs in Utility process");
    InitUtilityPDMs();
  } else if (XRE_IsContentProcess()) {
    PDM_INIT_LOG("Init PDMs in Content process");
    InitContentPDMs();
  } else {
    MOZ_DIAGNOSTIC_ASSERT(XRE_IsParentProcess(),
                          "PDMFactory is only usable in the "
                          "Parent/GPU/RDD/Utility/Content process");
    PDM_INIT_LOG("Init PDMs in Chrome process");
    InitDefaultPDMs();
  }
  sHasInitializedPDMs = true;
}

bool PDMInitializer::HasInitializedPDMs() {
  StaticMutexAutoLock mon(sMonitor);
  return sHasInitializedPDMs;
}

class SupportChecker {
 public:
  enum class Reason : uint8_t {
    kSupported,
    kVideoFormatNotSupported,
    kAudioFormatNotSupported,
    kUnknown,
  };

  struct CheckResult {
    explicit CheckResult(Reason aReason,
                         MediaResult aResult = MediaResult(NS_OK))
        : mReason(aReason), mMediaResult(std::move(aResult)) {}
    CheckResult(const CheckResult& aOther) = default;
    CheckResult(CheckResult&& aOther) = default;
    CheckResult& operator=(const CheckResult& aOther) = default;
    CheckResult& operator=(CheckResult&& aOther) = default;

    Reason mReason;
    MediaResult mMediaResult;
  };

  template <class Func>
  void AddToCheckList(Func&& aChecker) {
    mCheckerList.AppendElement(std::forward<Func>(aChecker));
  }

  void AddMediaFormatChecker(const TrackInfo& aTrackConfig) {
    if (aTrackConfig.IsVideo()) {
      auto mimeType = aTrackConfig.GetAsVideoInfo()->mMimeType;
      RefPtr<MediaByteBuffer> extraData =
          aTrackConfig.GetAsVideoInfo()->mExtraData;
      AddToCheckList(
          [mimeType = std::move(mimeType), extraData = std::move(extraData)]() {
            return CheckResult(SupportChecker::Reason::kSupported);
          });
    }
  }

  SupportChecker::CheckResult Check() {
    for (auto& checker : mCheckerList) {
      auto result = checker();
      if (result.mReason != SupportChecker::Reason::kSupported) {
        return result;
      }
    }
    return CheckResult(SupportChecker::Reason::kSupported);
  }

  void Clear() { mCheckerList.Clear(); }

 private:
  nsTArray<std::function<CheckResult()>> mCheckerList;
};  

PDMFactory::PDMFactory() {
  EnsureInit();
  CreatePDMs();
  CreateNullPDM();
}

PDMFactory::~PDMFactory() = default;

void PDMFactory::EnsureInit() {
  if (PDMInitializer::HasInitializedPDMs()) {
    return;
  }
  auto initalizationGfxVarsAndPreferences = []() {
    MOZ_DIAGNOSTIC_ASSERT(NS_IsMainThread());
    gfx::gfxVars::Initialize();
    (void)BrowserTabsRemoteAutostart();
  };
  if (!gfx::gfxVars::IsInitialized()) {
    if (NS_IsMainThread()) {
      initalizationGfxVarsAndPreferences();
    } else {
      nsCOMPtr<nsIEventTarget> mainTarget = GetMainThreadSerialEventTarget();
      nsCOMPtr<nsIRunnable> runnable =
          NS_NewRunnableFunction("PDMFactory::EnsureInit",
                                 std::move(initalizationGfxVarsAndPreferences));
      SyncRunnable::DispatchToThread(mainTarget, runnable);
    }
  }
  PDMInitializer::InitPDMs();
}

RefPtr<PlatformDecoderModule::CreateDecoderPromise> PDMFactory::CreateDecoder(
    const CreateDecoderParams& aParams) {
  if (aParams.mUseNullDecoder.mUse) {
    MOZ_ASSERT(mNullPDM);
    return CreateDecoderWithPDM(mNullPDM, aParams);
  }
  if (aParams.mConfig.mCrypto.IsEncrypted()) {
    return PlatformDecoderModule::CreateDecoderPromise::CreateAndReject(
        NS_ERROR_DOM_MEDIA_NOT_SUPPORTED_ERR, __func__);
  }

  return CheckAndMaybeCreateDecoder(CreateDecoderParamsForAsync(aParams), 0);
}

RefPtr<PlatformDecoderModule::CreateDecoderPromise>
PDMFactory::CheckAndMaybeCreateDecoder(CreateDecoderParamsForAsync&& aParams,
                                       uint32_t aIndex,
                                       Maybe<MediaResult> aEarlierError) {
  uint32_t i = aIndex;
  auto params = SupportDecoderParams(aParams);
  for (; i < mCurrentPDMs.Length(); i++) {
    if (mCurrentPDMs[i]->Supports(params, nullptr ).isEmpty()) {
      continue;
    }
    RefPtr<PlatformDecoderModule::CreateDecoderPromise> p =
        CreateDecoderWithPDM(mCurrentPDMs[i], aParams)
            ->Then(
                GetCurrentSerialEventTarget(), __func__,
                [](RefPtr<MediaDataDecoder>&& aDecoder) {
                  return PlatformDecoderModule::CreateDecoderPromise::
                      CreateAndResolve(std::move(aDecoder), __func__);
                },
                [self = RefPtr{this}, i, params = std::move(aParams)](
                    const MediaResult& aError) mutable {
                  return self->CheckAndMaybeCreateDecoder(std::move(params),
                                                          i + 1, Some(aError));
                });
    return p;
  }
  if (aEarlierError) {
    return PlatformDecoderModule::CreateDecoderPromise::CreateAndReject(
        std::move(*aEarlierError), __func__);
  }
  return PlatformDecoderModule::CreateDecoderPromise::CreateAndReject(
      MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                  nsPrintfCString("Error no decoder found for %s",
                                  aParams.mConfig->mMimeType.get())
                      .get()),
      __func__);
}

RefPtr<PlatformDecoderModule::CreateDecoderPromise>
PDMFactory::CreateDecoderWithPDM(PlatformDecoderModule* aPDM,
                                 const CreateDecoderParams& aParams) {
  MOZ_ASSERT(aPDM);
  MediaResult result = NS_OK;

  SupportChecker supportChecker;
  const TrackInfo& config = aParams.mConfig;
  supportChecker.AddMediaFormatChecker(config);

  auto checkResult = supportChecker.Check();
  if (checkResult.mReason != SupportChecker::Reason::kSupported) {
    if (checkResult.mReason ==
        SupportChecker::Reason::kVideoFormatNotSupported) {
      result = checkResult.mMediaResult;
    } else if (checkResult.mReason ==
               SupportChecker::Reason::kAudioFormatNotSupported) {
      result = checkResult.mMediaResult;
    }
    return PlatformDecoderModule::CreateDecoderPromise::CreateAndReject(
        result, __func__);
  }

  if (config.IsAudio()) {
    if (MP4Decoder::IsAAC(config.mMimeType) && !aParams.mUseNullDecoder.mUse &&
        aParams.mWrappers.contains(media::Wrapper::MediaChangeMonitor)) {
      return MediaChangeMonitor::Create(this, aParams);
    }
    RefPtr<PlatformDecoderModule::CreateDecoderPromise> p;
    p = aPDM->AsyncCreateDecoder(aParams)->Then(
        GetCurrentSerialEventTarget(), __func__,
        [params = CreateDecoderParamsForAsync(aParams)](
            RefPtr<MediaDataDecoder>&& aDecoder) {
          RefPtr<MediaDataDecoder> decoder = std::move(aDecoder);
          if (params.mWrappers.contains(media::Wrapper::AudioTrimmer)) {
            decoder = new AudioTrimmer(decoder.forget());
          }
          return PlatformDecoderModule::CreateDecoderPromise::CreateAndResolve(
              decoder, __func__);
        },
        [](const MediaResult& aError) {
          return PlatformDecoderModule::CreateDecoderPromise::CreateAndReject(
              aError, __func__);
        });
    return p;
  }

  if (!config.IsVideo()) {
    return PlatformDecoderModule::CreateDecoderPromise::CreateAndReject(
        MediaResult(
            NS_ERROR_DOM_MEDIA_FATAL_ERR,
            RESULT_DETAIL(
                "Decoder configuration error, expected audio or video.")),
        __func__);
  }

  if ((MP4Decoder::IsH264(config.mMimeType) ||
       AOMDecoder::IsAV1(config.mMimeType) ||
       VPXDecoder::IsVPX(config.mMimeType) ||
       MP4Decoder::IsHEVC(config.mMimeType)) &&
      !aParams.mUseNullDecoder.mUse &&
      aParams.mWrappers.contains(media::Wrapper::MediaChangeMonitor)) {
    return MediaChangeMonitor::Create(this, aParams);
  }
  return aPDM->AsyncCreateDecoder(aParams);
}

DecodeSupportSet PDMFactory::SupportsMimeType(
    const nsACString& aMimeType) const {
  UniquePtr<TrackInfo> trackInfo = CreateTrackInfoWithMIMEType(aMimeType);
  if (!trackInfo) {
    return DecodeSupportSet{};
  }
  return Supports(SupportDecoderParams(*trackInfo), nullptr);
}

DecodeSupportSet PDMFactory::Supports(
    const SupportDecoderParams& aParams,
    DecoderDoctorDiagnostics* aDiagnostics) const {
  if (aParams.mConfig.mCrypto.IsEncrypted()) {
    return DecodeSupportSet{};
  }

  RefPtr<PlatformDecoderModule> current =
      GetDecoderModule(aParams, aDiagnostics);

  if (!current) {
    return DecodeSupportSet{};
  }

  return current->Supports(aParams, aDiagnostics);
}

void PDMFactory::ForcePDM(PlatformDecoderModule* aPDM) {
  auto forced = sForcedPDM.Lock();
  *forced = aPDM;
}

void PDMFactory::CreatePDMs() {
  {
    auto forced = sForcedPDM.Lock();
    if (*forced) {
      StartupPDM(do_AddRef(*forced));
      return;
    }
  }
  if (StaticPrefs::media_use_blank_decoder()) {
    StartupPDM(BlankDecoderModule::Create());
    return;
  }

  if (XRE_IsGPUProcess()) {
    CreateGpuPDMs();
  } else if (XRE_IsRDDProcess()) {
    CreateRddPDMs();
  } else if (XRE_IsUtilityProcess()) {
    CreateUtilityPDMs();
  } else if (XRE_IsContentProcess()) {
    CreateContentPDMs();
  } else {
    MOZ_DIAGNOSTIC_ASSERT(
        XRE_IsParentProcess(),
        "PDMFactory is only usable in the Parent/GPU/RDD/Content process");
    CreateDefaultPDMs();
  }
}

void PDMFactory::CreateGpuPDMs() {
}

#if defined(MOZ_FFMPEG)
static DecoderDoctorDiagnostics::Flags GetFailureFlagBasedOnFFmpegStatus(
    const FFmpegRuntimeLinker::LinkStatus& aStatus) {
  switch (aStatus) {
    case FFmpegRuntimeLinker::LinkStatus_INVALID_FFMPEG_CANDIDATE:
    case FFmpegRuntimeLinker::LinkStatus_UNUSABLE_LIBAV57:
    case FFmpegRuntimeLinker::LinkStatus_INVALID_LIBAV_CANDIDATE:
    case FFmpegRuntimeLinker::LinkStatus_OBSOLETE_FFMPEG:
    case FFmpegRuntimeLinker::LinkStatus_OBSOLETE_LIBAV:
    case FFmpegRuntimeLinker::LinkStatus_INVALID_CANDIDATE:
      return DecoderDoctorDiagnostics::Flags::LibAVCodecUnsupported;
    default:
      MOZ_DIAGNOSTIC_ASSERT(
          aStatus == FFmpegRuntimeLinker::LinkStatus_NOT_FOUND,
          "Only call this method when linker fails.");
      return DecoderDoctorDiagnostics::Flags::FFmpegNotFound;
  }
}
#endif

void PDMFactory::CreateRddPDMs() {
  StartupPDM(FFVPXRuntimeLinker::CreateDecoder());
#if defined(MOZ_FFMPEG)
  if (StaticPrefs::media_ffmpeg_enabled() &&
      StaticPrefs::media_rdd_ffmpeg_enabled() &&
      !StartupPDM(
          FFmpegRuntimeLinker::CreateDecoder(),
#if defined(MOZ_WIDGET_GTK)
          StaticPrefs::media_hardware_video_decoding_vulkan_enabled_AtStartup()
#else
          false
#endif
              )) {
    mFailureFlags += GetFailureFlagBasedOnFFmpegStatus(
        FFmpegRuntimeLinker::LinkStatusCode());
  }
#endif
  StartupPDM(AgnosticDecoderModule::Create(),
             StaticPrefs::media_prefer_non_ffvpx());

  PDM_INIT_LOG("RDD PDM order:");
  int i = 0;
  for (const auto& pdm : mCurrentPDMs) {
    PDM_INIT_LOG("{}: {}", i++, pdm->Name());
  }
}

void PDMFactory::CreateUtilityPDMs() {
  const ipc::UtilityProcessKind aKind = GetCurrentUtilityProcessKind();
  if (aKind == ipc::UtilityProcessKind::GENERIC_UTILITY) {
    StartupPDM(FFVPXRuntimeLinker::CreateDecoder());
#if defined(MOZ_FFMPEG)
    if (StaticPrefs::media_ffmpeg_enabled() &&
        !StartupPDM(FFmpegRuntimeLinker::CreateDecoder())) {
      mFailureFlags += GetFailureFlagBasedOnFFmpegStatus(
          FFmpegRuntimeLinker::LinkStatusCode());
    }
#endif
    StartupPDM(AgnosticDecoderModule::Create(),
               StaticPrefs::media_prefer_non_ffvpx());
  }
  PDM_INIT_LOG("Utility PDM order:");
  int i = 0;
  for (const auto& pdm : mCurrentPDMs) {
    PDM_INIT_LOG("{}: {}", i++, pdm->Name());
  }
}

void PDMFactory::CreateContentPDMs() {
  if (StaticPrefs::media_gpu_process_decoder()) {
    StartupPDM(RemoteDecoderModule::Create(RemoteMediaIn::GpuProcess));
  }

  if (StaticPrefs::media_rdd_process_enabled()) {
    StartupPDM(RemoteDecoderModule::Create(RemoteMediaIn::RddProcess));
  }

  if (StaticPrefs::media_utility_process_enabled()) {
    StartupPDM(
        RemoteDecoderModule::Create(RemoteMediaIn::UtilityProcess_Generic));
  }
#if !0  // Still required for video?
  if (StaticPrefs::media_allow_audio_non_utility()) {
#endif

#if defined(MOZ_OMX)
    if (StaticPrefs::media_omx_enabled()) {
      StartupPDM(OmxDecoderModule::Create());
    }
#endif
    StartupPDM(FFVPXRuntimeLinker::CreateDecoder());
#if defined(MOZ_FFMPEG)
    if (StaticPrefs::media_ffmpeg_enabled() &&
        !StartupPDM(FFmpegRuntimeLinker::CreateDecoder())) {
      mFailureFlags += GetFailureFlagBasedOnFFmpegStatus(
          FFmpegRuntimeLinker::LinkStatusCode());
    }
#endif

    StartupPDM(AgnosticDecoderModule::Create(),
               StaticPrefs::media_prefer_non_ffvpx());
#if !0  // Still required for video?
  }
#endif


  PDM_INIT_LOG("Content PDM order:");
  int i = 0;
  for (const auto& pdm : mCurrentPDMs) {
    PDM_INIT_LOG("{}: {}", i++, pdm->Name());
  }
}

void PDMFactory::CreateDefaultPDMs() {

#if defined(MOZ_OMX)
  if (StaticPrefs::media_omx_enabled()) {
    StartupPDM(OmxDecoderModule::Create());
  }
#endif
  StartupPDM(FFVPXRuntimeLinker::CreateDecoder());
#if defined(MOZ_FFMPEG)
  if (StaticPrefs::media_ffmpeg_enabled() &&
      !StartupPDM(FFmpegRuntimeLinker::CreateDecoder())) {
    mFailureFlags += GetFailureFlagBasedOnFFmpegStatus(
        FFmpegRuntimeLinker::LinkStatusCode());
  }
#endif

  StartupPDM(AgnosticDecoderModule::Create(),
             StaticPrefs::media_prefer_non_ffvpx());

  PDM_INIT_LOG("Default PDM order:");
  int i = 0;
  for (const auto& pdm : mCurrentPDMs) {
    PDM_INIT_LOG("{}: {}", i++, pdm->Name());
  }
}

void PDMFactory::CreateNullPDM() {
  mNullPDM = CreateNullDecoderModule();
  MOZ_ASSERT(mNullPDM && NS_SUCCEEDED(mNullPDM->Startup()));
}

bool PDMFactory::StartupPDM(already_AddRefed<PlatformDecoderModule> aPDM,
                            bool aInsertAtBeginning) {
  RefPtr<PlatformDecoderModule> pdm = aPDM;
  if (pdm && NS_SUCCEEDED(pdm->Startup())) {
    if (aInsertAtBeginning) {
      mCurrentPDMs.InsertElementAt(0, pdm);
    } else {
      mCurrentPDMs.AppendElement(pdm);
    }
    return true;
  }
  return false;
}

already_AddRefed<PlatformDecoderModule> PDMFactory::GetDecoderModule(
    const SupportDecoderParams& aParams,
    DecoderDoctorDiagnostics* aDiagnostics) const {
  if (aDiagnostics) {
    aDiagnostics->SetFailureFlags(mFailureFlags);
  }

  RefPtr<PlatformDecoderModule> pdm;
  for (const auto& current : mCurrentPDMs) {
    if (!current->Supports(aParams, aDiagnostics).isEmpty()) {
      pdm = current;
      break;
    }
  }
  return pdm.forget();
}

StaticMutex PDMFactory::sSupportedMutex;

media::MediaCodecsSupported PDMFactory::Supported(bool aForceRefresh) {
  StaticMutexAutoLock lock(sSupportedMutex);

  if (aForceRefresh) {
    PDMFactorySupport::Invalidate();
  }

  auto calculate = []() {
    MediaCodecsSupported supported;
    for (const auto& cd : MCSInfo::GetAllCodecDefinitions()) {
      supported += MCSInfo::GetDecodeMediaCodecsSupported(
          cd.codec,
          PDMFactorySupport::IsTypeSupported(nsCString(cd.mimeTypeString)));
    }
    return supported;
  };

  static MediaCodecsSupported supported = calculate();
  if (aForceRefresh) {
    supported = calculate();
  }

  return supported;
}

DecodeSupportSet PDMFactory::SupportsMimeType(
    const nsACString& aMimeType, const MediaCodecsSupported& aSupported,
    RemoteMediaIn aLocation) {
  const TrackSupportSet supports =
      RemoteMediaManagerChild::GetTrackSupport(aLocation);

  if (supports.contains(TrackSupport::DecodeVideo)) {
    if (MP4Decoder::IsH264(aMimeType)) {
      return MCSInfo::GetDecodeSupportSet(MediaCodec::H264, aSupported);
    }
    if (VPXDecoder::IsVP9(aMimeType)) {
      return MCSInfo::GetDecodeSupportSet(MediaCodec::VP9, aSupported);
    }
    if (VPXDecoder::IsVP8(aMimeType)) {
      return MCSInfo::GetDecodeSupportSet(MediaCodec::VP8, aSupported);
    }
    if (AOMDecoder::IsAV1(aMimeType)) {
      return MCSInfo::GetDecodeSupportSet(MediaCodec::AV1, aSupported);
    }
    if (MP4Decoder::IsHEVC(aMimeType)) {
      return MCSInfo::GetDecodeSupportSet(MediaCodec::HEVC, aSupported);
    }
  }

  if (supports.contains(TrackSupport::DecodeAudio)) {
    if (MP4Decoder::IsAAC(aMimeType)) {
      return MCSInfo::GetDecodeSupportSet(MediaCodec::AAC, aSupported);
    }
    if (aMimeType.EqualsLiteral("audio/mpeg")) {
      return MCSInfo::GetDecodeSupportSet(MediaCodec::MP3, aSupported);
    }
    if (aMimeType.EqualsLiteral("audio/opus")) {
      return MCSInfo::GetDecodeSupportSet(MediaCodec::Opus, aSupported);
    }
    if (aMimeType.EqualsLiteral("audio/vorbis")) {
      return MCSInfo::GetDecodeSupportSet(MediaCodec::Vorbis, aSupported);
    }
    if (aMimeType.EqualsLiteral("audio/flac")) {
      return MCSInfo::GetDecodeSupportSet(MediaCodec::FLAC, aSupported);
    }
    if (IsWaveMimetype(aMimeType)) {
      return MCSInfo::GetDecodeSupportSet(MediaCodec::Wave, aSupported);
    }
  }
  return DecodeSupportSet{};
}

bool PDMFactory::AllDecodersAreRemote() {
  return StaticPrefs::media_rdd_process_enabled() &&
         StaticPrefs::media_rdd_opus_enabled() &&
         StaticPrefs::media_rdd_vorbis_enabled() &&
         StaticPrefs::media_rdd_vpx_enabled() &&
         StaticPrefs::media_rdd_wav_enabled();
}

#undef PDM_INIT_LOG
}  
