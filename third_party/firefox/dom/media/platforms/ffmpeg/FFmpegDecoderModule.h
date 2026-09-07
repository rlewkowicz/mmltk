/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(FFmpegDecoderModule_h_)
#define FFmpegDecoderModule_h_

#include "FFmpegAudioDecoder.h"
#include "FFmpegLibWrapper.h"
#include "FFmpegUtils.h"
#include "PlatformDecoderModule.h"
#include "mozilla/DataMutex.h"

#if !defined(MOZ_FFVPX_AUDIOONLY)
#  include "FFmpegVideoDecoder.h"
#  include "MP4Decoder.h"
#  include "VPXDecoder.h"
#  include "VideoUtils.h"
#  include "mozilla/StaticPrefs_media.h"
#  include "mozilla/gfx/gfxVars.h"
#  include "prenv.h"
#endif

#if defined(DEBUG)
#  include "mozilla/AppShutdown.h"
#endif

namespace mozilla {

template <int V>
class FFmpegDecoderModule : public PlatformDecoderModule {
 public:
  const char* Name() const override {
#if defined(FFVPX_VERSION)
    return "FFmpeg(FFVPX)";
#else
    return "FFmpeg(OS library)";
#endif
  }
  static void Init(const FFmpegLibWrapper* aLib) {
#if (0 || defined(MOZ_WIDGET_GTK) || \
     0) &&               \
    defined(MOZ_USE_HWDECODE) && !defined(MOZ_FFVPX_AUDIOONLY)
    if (!XRE_IsRDDProcess() && !XRE_IsUtilityProcess()
    ) {
      return;
    }

    if (!gfx::gfxVars::IsInitialized()) {
      MOZ_ASSERT(AppShutdown::IsInOrBeyond(ShutdownPhase::XPCOMShutdown));
      return;
    }

    const AVHWDeviceType kDeviceTypes[] = {
#if defined(MOZ_WIDGET_GTK)
#if LIBAVCODEC_VERSION_MAJOR >= 60
        AV_HWDEVICE_TYPE_VULKAN,
#endif
        AV_HWDEVICE_TYPE_VAAPI,
        AV_HWDEVICE_TYPE_NONE,  
#endif
    };

    struct CodecEntry {
      AVCodecID mId;
      bool mHwAllowed;
    };

    const CodecEntry kCodecIDs[] = {
#if LIBAVCODEC_VERSION_MAJOR >= 59
        {AV_CODEC_ID_AV1, gfx::gfxVars::UseAV1HwDecode()},
#endif
#if LIBAVCODEC_VERSION_MAJOR >= 55
        {AV_CODEC_ID_VP9, gfx::gfxVars::UseVP9HwDecode()},
#endif
#if (defined(MOZ_WIDGET_GTK) || 0) && \
      LIBAVCODEC_VERSION_MAJOR >= 54
        {AV_CODEC_ID_VP8, gfx::gfxVars::UseVP8HwDecode()},
#endif

#if defined(MOZ_WIDGET_GTK) && !defined(FFVPX_VERSION)
#if LIBAVCODEC_VERSION_MAJOR >= 55
        {AV_CODEC_ID_HEVC, gfx::gfxVars::UseHEVCHwDecode()},
#endif
        {AV_CODEC_ID_H264, gfx::gfxVars::UseH264HwDecode()},
#endif
    };

    {
      auto hwCodecs = sSupportedHWCodecs.Lock();
      hwCodecs->Clear();
      for (const auto& entry : kCodecIDs) {
        if (!entry.mHwAllowed) {
          MOZ_LOG_FMT(sPDMLog, LogLevel::Debug,
                      "Hw codec disabled by gfxVars for {}",
                      AVCodecToString(entry.mId));
          continue;
        }

        const AVCodec* codec = nullptr;
        for (const auto& deviceType : kDeviceTypes) {
          codec = FFmpegVideoDecoder<V>::FindVideoHardwareAVCodec(
              aLib, entry.mId, deviceType);
          if (codec) {
            break;
          }
        }

        if (!codec) {
          MOZ_LOG_FMT(sPDMLog, LogLevel::Debug, "No hw codec or decoder for {}",
                      AVCodecToString(entry.mId));
          continue;
        }

        hwCodecs->AppendElement(entry.mId);
        MOZ_LOG_FMT(sPDMLog, LogLevel::Debug, "Support {} for hw decoding",
                    AVCodecToString(entry.mId));
      }
    }
#endif
  }

  static already_AddRefed<PlatformDecoderModule> Create(
      const FFmpegLibWrapper* aLib) {
    RefPtr<PlatformDecoderModule> pdm = new FFmpegDecoderModule(aLib);

    return pdm.forget();
  }

  explicit FFmpegDecoderModule(const FFmpegLibWrapper* aLib) : mLib(aLib) {}
  virtual ~FFmpegDecoderModule() = default;

  already_AddRefed<MediaDataDecoder> CreateVideoDecoder(
      const CreateDecoderParams& aParams) override {
#if defined(MOZ_FFVPX_AUDIOONLY)
    return nullptr;
#else
    if (Supports(SupportDecoderParams(aParams), nullptr).isEmpty()) {
      return nullptr;
    }
    auto decoder = MakeRefPtr<FFmpegVideoDecoder<V>>(
        mLib, aParams.VideoConfig(), aParams.mKnowsCompositor,
        aParams.mImageContainer,
        aParams.mOptions.contains(CreateDecoderParams::Option::LowLatency),
        aParams.mOptions.contains(
            CreateDecoderParams::Option::HardwareDecoderNotAllowed),
        aParams.mOptions.contains(
            CreateDecoderParams::Option::Output8BitPerChannel),
        aParams.mTrackingId);

    return decoder.forget();
#endif
  }

  already_AddRefed<MediaDataDecoder> CreateAudioDecoder(
      const CreateDecoderParams& aParams) override {
    if (XRE_IsGPUProcess()) {
      return nullptr;
    }
    if (Supports(SupportDecoderParams(aParams), nullptr).isEmpty()) {
      return nullptr;
    }
    RefPtr<MediaDataDecoder> decoder = new FFmpegAudioDecoder<V>(mLib, aParams);
    return decoder.forget();
  }

  media::DecodeSupportSet SupportsMimeType(
      const nsACString& aMimeType,
      DecoderDoctorDiagnostics* aDiagnostics) const override {
    UniquePtr<TrackInfo> trackInfo = CreateTrackInfoWithMIMEType(aMimeType);
    if (!trackInfo) {
      return media::DecodeSupportSet{};
    }
    return Supports(SupportDecoderParams(*trackInfo), aDiagnostics);
  }

  media::DecodeSupportSet Supports(
      const SupportDecoderParams& aParams,
      DecoderDoctorDiagnostics* aDiagnostics) const override {
    const auto& trackInfo = aParams.mConfig;
    const nsACString& mimeType = trackInfo.mMimeType;
#if defined(MOZ_FFVPX_AUDIOONLY)
    if (trackInfo.GetAsVideoInfo()) {
      return media::DecodeSupportSet{};
    }
#else
    if (XRE_IsGPUProcess() && !trackInfo.GetAsVideoInfo()) {
      return media::DecodeSupportSet{};
    }

    if (VPXDecoder::IsVPX(mimeType) && trackInfo.GetAsVideoInfo()->HasAlpha()) {
      MOZ_LOG_FMT(sPDMLog, LogLevel::Debug,
                  "FFmpeg decoder rejects requested type '{}'",
                  PromiseFlatCString(mimeType).get());
      return media::DecodeSupportSet{};
    }

    if (VPXDecoder::IsVP9(mimeType) &&
        aParams.mOptions.contains(CreateDecoderParams::Option::LowLatency)) {
      MOZ_LOG_FMT(
          sPDMLog, LogLevel::Debug,
          "FFmpeg decoder rejects requested type '{}' due to low latency",
          PromiseFlatCString(mimeType).get());
      return media::DecodeSupportSet{};
    }

    if (MP4Decoder::IsHEVC(mimeType) && !StaticPrefs::media_hevc_enabled()) {
      MOZ_LOG_FMT(
          sPDMLog, LogLevel::Debug,
          "FFmpeg decoder rejects requested type '{}' due to being disabled "
          "by the pref",
          PromiseFlatCString(mimeType).get());
      return media::DecodeSupportSet{};
    }
#endif

    AVCodecID audioCodec = FFmpegAudioDecoder<V>::GetCodecId(
        mimeType,
        trackInfo.GetAsAudioInfo() ? *trackInfo.GetAsAudioInfo() : AudioInfo());
#if defined(MOZ_FFVPX_AUDIOONLY)
    if (audioCodec == AV_CODEC_ID_NONE) {
#else
    AVCodecID videoCodec = FFmpegVideoDecoder<V>::GetCodecId(mimeType);
    if (audioCodec == AV_CODEC_ID_NONE && videoCodec == AV_CODEC_ID_NONE) {
#endif
      MOZ_LOG_FMT(sPDMLog, LogLevel::Debug,
                  "FFmpeg decoder rejects requested type '{}'",
                  PromiseFlatCString(mimeType).get());
      return media::DecodeSupportSet{};
    }
#if defined(MOZ_FFVPX_AUDIOONLY)
    AVCodecID codecId = audioCodec;
#else
    AVCodecID codecId =
        audioCodec != AV_CODEC_ID_NONE ? audioCodec : videoCodec;
#endif

    media::DecodeSupportSet supports;
    if (IsSWDecodingSupported(codecId)) {
      supports += media::DecodeSupport::SoftwareDecode;
    }
    if (IsHWDecodingSupported(codecId)) {
      supports += media::DecodeSupport::HardwareDecode;
    }


    MOZ_LOG_FMT(sPDMLog, LogLevel::Debug,
                "FFmpeg decoder {} requested type '{}'",
                supports.isEmpty() ? "rejects" : "supports",
                PromiseFlatCString(mimeType).get());
    return supports;
  }

 protected:
  bool SupportsColorDepth(
      gfx::ColorDepth aColorDepth,
      DecoderDoctorDiagnostics* aDiagnostics) const override {
    return true;
  }

  bool IsSWDecodingSupported(const AVCodecID& aCodec) const {
    return FFmpegDataDecoder<V>::FindSoftwareAVCodec(mLib, aCodec);
  }

  bool IsHWDecodingSupported(AVCodecID aCodec) const {
#if defined(FFVPX_VERSION)
    if (!StaticPrefs::media_ffvpx_hw_enabled()) {
      return false;
    }
#endif
    auto hwCodecs = sSupportedHWCodecs.Lock();
    return hwCodecs->Contains(aCodec);
  }

 private:
  const FFmpegLibWrapper* mLib;
  constinit static inline StaticDataMutex<nsTArray<AVCodecID>>
      sSupportedHWCodecs{"sSupportedHWCodecs"};
};

}  

#endif
