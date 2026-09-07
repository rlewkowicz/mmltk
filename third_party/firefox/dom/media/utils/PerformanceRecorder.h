/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_PerformanceRecorder_h
#define mozilla_PerformanceRecorder_h

#include <cstdint>
#include <utility>

#include "mozilla/DefineEnum.h"
#include "mozilla/Maybe.h"
#include "mozilla/TypedEnumBits.h"
#include "nsString.h"

namespace mozilla {
namespace gfx {
enum class YUVColorSpace : uint8_t;
enum class ColorDepth : uint8_t;
enum class ColorRange : uint8_t;
}  

struct TrackingId {
  MOZ_DEFINE_ENUM_CLASS_WITH_BASE_AND_TOSTRING_AT_CLASS_SCOPE(
      Source, uint8_t,
      (Unimplemented, AudioDestinationNode, Camera, Canvas, ChannelDecoder,
       HLSDecoder, MediaCapabilities, MediaElementDecoder, MediaElementStream,
       MSEDecoder, RTCRtpReceiver, Screen, Tab, Window, LAST));
  enum class TrackAcrossProcesses : uint8_t { Yes, No };

  TrackingId();
  TrackingId(Source aSource, uint32_t aUniqueInProcId,
             TrackAcrossProcesses aTrack = TrackAcrossProcesses::No);

  nsCString ToString() const;

  Source mSource;
  uint32_t mUniqueInProcId;
  Maybe<uint32_t> mProcId;
};

enum class MediaInfoFlag : uint16_t {
  None = 0,
  NonKeyFrame = 1 << 0,
  KeyFrame = 1 << 1,
  SoftwareDecoding = 1 << 2,
  HardwareDecoding = 1 << 3,
  VIDEO_AV1 = 1 << 4,
  VIDEO_H264 = 1 << 5,
  VIDEO_VP8 = 1 << 6,
  VIDEO_VP9 = 1 << 7,
  VIDEO_HEVC = 1 << 9,
};
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(MediaInfoFlag)

MOZ_DEFINE_ENUM_CLASS_WITH_BASE_AND_TOSTRING(
    MediaStage, uint8_t,
    (Invalid, RequestData, RequestDemux, CopyDemuxedData, RequestDecode,
     CopyDecodedVideo));

class PlaybackStage {
 public:
  explicit PlaybackStage(MediaStage, int32_t = 0,
                         MediaInfoFlag = MediaInfoFlag::None) {}
  void SetStartTimeAndEndTime(uint64_t, uint64_t) {}
  void AddFlag(MediaInfoFlag) {}
};

class CaptureStage {
 public:
  MOZ_DEFINE_ENUM_CLASS_WITH_BASE_AND_TOSTRING_AT_CLASS_SCOPE(
      ImageType, uint8_t, (Unknown, I420, YUY2, YV12, UYVY, NV12, NV21, MJPEG));

  CaptureStage(nsCString, TrackingId, int32_t, int32_t, ImageType) {}
};

class CopyVideoStage {
 public:
  CopyVideoStage(nsCString, TrackingId, int32_t, int32_t) {}
};

class DecodeStage {
 public:
  MOZ_DEFINE_ENUM_WITH_BASE_AND_TOSTRING_AT_CLASS_SCOPE(
      ImageFormat, uint8_t,
      (YUV420P, YUV422P, YUV444P, NV12, YV12, NV21, P010, P016, RGBA32, RGB24,
       GBRP, ANDROID_SURFACE, VAAPI_SURFACE, D3D11_SURFACE));

  DecodeStage(nsCString, TrackingId, MediaInfoFlag) {}
  void SetResolution(int, int) {}
  void SetImageFormat(ImageFormat) {}
  void SetYUVColorSpace(gfx::YUVColorSpace) {}
  void SetColorRange(gfx::ColorRange) {}
  void SetColorDepth(gfx::ColorDepth) {}
  void SetStartTimeAndEndTime(uint64_t, uint64_t) {}
};

template <typename StageType>
class PerformanceRecorder {
 public:
  template <typename... Args>
  explicit PerformanceRecorder(Args&&...) {}

  PerformanceRecorder(PerformanceRecorder&&) noexcept = default;
  PerformanceRecorder& operator=(PerformanceRecorder&&) = default;
  PerformanceRecorder(const PerformanceRecorder&) = delete;
  PerformanceRecorder& operator=(const PerformanceRecorder&) = delete;

  template <typename F>
  float Record(F&&) {
    return 0.0f;
  }
  float Record() { return 0.0f; }
};

template <typename StageType>
class PerformanceRecorderMulti {
 public:
  PerformanceRecorderMulti() = default;
  PerformanceRecorderMulti(PerformanceRecorderMulti&&) noexcept = default;
  PerformanceRecorderMulti& operator=(PerformanceRecorderMulti&&) = default;
  PerformanceRecorderMulti(const PerformanceRecorderMulti&) = delete;
  PerformanceRecorderMulti& operator=(const PerformanceRecorderMulti&) = delete;

  template <typename... Args>
  void Start(int64_t, Args&&...) {}

  template <typename F>
  float Record(int64_t, F&&) {
    return 0.0f;
  }
  float Record(int64_t) { return 0.0f; }
};

}  

#endif  // mozilla_PerformanceRecorder_h
