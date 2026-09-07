/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FFmpegEncoderModule.h"

#include "EncoderConfig.h"
#include "FFmpegAudioEncoder.h"
#include "FFmpegLog.h"
#include "FFmpegUtils.h"

#if !defined(MOZ_FFVPX_AUDIOONLY)
#  include "FFmpegVideoEncoder.h"
#endif

#if defined(DEBUG)
#  include "mozilla/AppShutdown.h"
#endif

#include "mozilla/StaticPrefs_media.h"
#include "mozilla/gfx/gfxVars.h"
#include "prenv.h"

#include "FFmpegLibs.h"

using mozilla::media::EncodeSupport;
using mozilla::media::EncodeSupportSet;

namespace mozilla {

template <int V>
 void FFmpegEncoderModule<V>::Init(const FFmpegLibWrapper* aLib) {
#if (0 || defined(MOZ_WIDGET_GTK) ||                \
     0) &&                              \
    defined(MOZ_USE_HWDECODE) && !defined(MOZ_FFVPX_AUDIOONLY) && \
    LIBAVCODEC_VERSION_MAJOR >= 58
  if (!XRE_IsRDDProcess())
  {
    MOZ_LOG_FMT(sPEMLog, LogLevel::Debug, "No support in {} process",
                XRE_GetProcessTypeString());
    return;
  }

  if (!gfx::gfxVars::IsInitialized()) {
    MOZ_ASSERT(AppShutdown::IsInOrBeyond(ShutdownPhase::XPCOMShutdown));
    return;
  }

  struct CodecEntry {
    AVCodecID mId;
    bool mHwAllowed;
  };

  const CodecEntry kCodecIDs[] = {
#if LIBAVCODEC_VERSION_MAJOR >= 59
      {AV_CODEC_ID_AV1, gfx::gfxVars::UseAV1HwEncode()},
#endif
      {AV_CODEC_ID_VP9, gfx::gfxVars::UseVP9HwEncode()},
#if defined(MOZ_WIDGET_GTK) || 0
      {AV_CODEC_ID_VP8, gfx::gfxVars::UseVP8HwEncode()},
#endif

#if defined(MOZ_WIDGET_GTK) && !defined(FFVPX_VERSION)
      {AV_CODEC_ID_HEVC, gfx::gfxVars::UseHEVCHwEncode()},
      {AV_CODEC_ID_H264, gfx::gfxVars::UseH264HwEncode()},
#endif
  };

  auto hwCodecs = sSupportedHWCodecs.Lock();
  hwCodecs->Clear();
  for (const auto& entry : kCodecIDs) {
    if (!entry.mHwAllowed) {
      MOZ_LOG_FMT(sPEMLog, LogLevel::Debug,
                  "Hw codec disabled by gfxVars for {}",
                  AVCodecToString(entry.mId));
      continue;
    }

    const auto* codec =
        FFmpegDataEncoder<V>::FindHardwareEncoder(aLib, entry.mId);
    if (!codec) {
      MOZ_LOG_FMT(sPEMLog, LogLevel::Debug, "No hw codec or encoder for {}",
                  AVCodecToString(entry.mId));
      continue;
    }

    hwCodecs->AppendElement(entry.mId);
    MOZ_LOG_FMT(sPEMLog, LogLevel::Debug, "Support {} for hw encoding",
                AVCodecToString(entry.mId));
  }
#endif
}  

template <int V>
EncodeSupportSet FFmpegEncoderModule<V>::Supports(
    const EncoderConfig& aConfig) const {
#if defined(MOZ_FFVPX_AUDIOONLY)
  if (!aConfig.IsAudio()) {
    return EncodeSupportSet{};
  }
#endif
  if (!CanLikelyEncode(aConfig)) {
    return EncodeSupportSet{};
  }
  if ((aConfig.mScalabilityMode != ScalabilityMode::None)) {
    if (aConfig.mCodec == CodecType::AV1) {
      if (aConfig.mBitrateMode != BitrateMode::Constant) {
        return EncodeSupportSet{};
      }
    } else if (aConfig.mCodec != CodecType::VP8 &&
               aConfig.mCodec != CodecType::VP9) {
      return EncodeSupportSet{};
    }
  }
  auto support = SupportsCodec(aConfig.mCodec);
  if (aConfig.mHardwarePreference == HardwarePreference::RequireHardware &&
      !support.contains(EncodeSupport::HardwareEncode)) {
    return {};
  }
  if (aConfig.mHardwarePreference == HardwarePreference::RequireSoftware &&
      !support.contains(EncodeSupport::SoftwareEncode)) {
    return {};
  }
  return support;
}

template <int V>
EncodeSupportSet FFmpegEncoderModule<V>::SupportsCodec(CodecType aCodec) const {
  AVCodecID id = GetFFmpegEncoderCodecId<V>(aCodec);
  if (id == AV_CODEC_ID_NONE) {
    return EncodeSupportSet{};
  }
#if LIBAVCODEC_VERSION_MAJOR >= 58
  if (id == AV_CODEC_ID_HEVC && !StaticPrefs::media_hevc_enabled()) {
    return EncodeSupportSet{};
  }
#endif
  EncodeSupportSet supports;
#if defined(MOZ_USE_HWDECODE)
  if (StaticPrefs::media_ffvpx_hw_enabled()) {
    auto hwCodecs = sSupportedHWCodecs.Lock();
    if (hwCodecs->Contains(static_cast<uint32_t>(id))) {
      supports += EncodeSupport::HardwareEncode;
    }
  }
#endif
  if (FFmpegDataEncoder<V>::FindSoftwareEncoder(mLib, id)) {
    supports += EncodeSupport::SoftwareEncode;
  }
  return supports;
}

template <int V>
already_AddRefed<MediaDataEncoder> FFmpegEncoderModule<V>::CreateVideoEncoder(
    const EncoderConfig& aConfig, const RefPtr<TaskQueue>& aTaskQueue) const {
#if defined(MOZ_FFVPX_AUDIOONLY)
  return nullptr;
#else
  AVCodecID codecId = GetFFmpegEncoderCodecId<V>(aConfig.mCodec);
  if (codecId == AV_CODEC_ID_NONE) {
    FFMPEGV_LOG("No ffmpeg encoder for {}", EnumValueToString(aConfig.mCodec));
    return nullptr;
  }

  RefPtr<MediaDataEncoder> encoder =
      new FFmpegVideoEncoder<V>(mLib, codecId, aTaskQueue, aConfig);
  FFMPEGV_LOG("ffmpeg {} encoder: {} has been created",
              EnumValueToString(aConfig.mCodec),
              encoder->GetDescriptionName().get());
  return encoder.forget();
#endif
}

template <int V>
already_AddRefed<MediaDataEncoder> FFmpegEncoderModule<V>::CreateAudioEncoder(
    const EncoderConfig& aConfig, const RefPtr<TaskQueue>& aTaskQueue) const {
  AVCodecID codecId = GetFFmpegEncoderCodecId<V>(aConfig.mCodec);
  if (codecId == AV_CODEC_ID_NONE) {
    FFMPEGV_LOG("No ffmpeg encoder for {}", EnumValueToString(aConfig.mCodec));
    return nullptr;
  }

  RefPtr<MediaDataEncoder> encoder =
      new FFmpegAudioEncoder<V>(mLib, codecId, aTaskQueue, aConfig);
  FFMPEGA_LOG("ffmpeg {} encoder: {} has been created",
              EnumValueToString(aConfig.mCodec),
              encoder->GetDescriptionName().get());
  return encoder.forget();
}

template class FFmpegEncoderModule<LIBAV_VER>;

}  
