/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "LayersTypes.h"

#include <cinttypes>
#include "nsPrintfCString.h"
#include "mozilla/gfx/gfxVars.h"


namespace mozilla {
namespace layers {

const char* kCompositionPayloadTypeNames[kCompositionPayloadTypeCount] = {
    "KeyPress",
    "APZScroll",
    "APZPinchZoom",
    "ContentPaint",
    "MouseUpFollowedByClick",
};

const char* GetLayersBackendName(LayersBackend aBackend) {
  switch (aBackend) {
    case LayersBackend::LAYERS_NONE:
      return "none";
    case LayersBackend::LAYERS_WR:
      return "webrender";
    default:
      MOZ_ASSERT_UNREACHABLE("unknown layers backend");
      return "unknown";
  }
}

std::ostream& operator<<(std::ostream& aStream, const LayersId& aId) {
  return aStream << nsPrintfCString("0x%" PRIx64, aId.mId).get();
}

CompositableHandle CompositableHandle::GetNext() {
  static std::atomic<uint64_t> sCounter = 0;
  return CompositableHandle{++sCounter};
}

RemoteTextureId RemoteTextureId::GetNext() {
  static std::atomic<uint64_t> sCounter = 0;
  return RemoteTextureId{++sCounter};
}

RemoteTextureOwnerId RemoteTextureOwnerId::GetNext() {
  static std::atomic<uint64_t> sCounter = 0;
  return RemoteTextureOwnerId{++sCounter};
}

SurfaceDescriptorRemoteDecoderId SurfaceDescriptorRemoteDecoderId::GetNext() {
  static std::atomic<uint64_t> sCounter = 0;
  return SurfaceDescriptorRemoteDecoderId{++sCounter};
}

GpuProcessTextureId GpuProcessTextureId::GetNext() {
  if (!XRE_IsGPUProcess()) {
    MOZ_ASSERT_UNREACHABLE("unexpected to be called");
    return GpuProcessTextureId{};
  }

  static std::atomic<uint64_t> sCounter = 0;
  return GpuProcessTextureId{++sCounter};
}

CompositeProcessFencesHolderId CompositeProcessFencesHolderId::GetNext() {
  if (!XRE_IsGPUProcess()) {
    MOZ_ASSERT_UNREACHABLE("unexpected to be called");
    return CompositeProcessFencesHolderId{};
  }

  static std::atomic<uint64_t> sCounter = 0;
  return CompositeProcessFencesHolderId{++sCounter};
}

GpuProcessAndroidImageReaderId GpuProcessAndroidImageReaderId::GetNext() {
  if (!XRE_IsGPUProcess()) {
    MOZ_ASSERT_UNREACHABLE("unexpected to be called");
    return GpuProcessAndroidImageReaderId{};
  }

  static std::atomic<uint64_t> sCounter = 0;
  return GpuProcessAndroidImageReaderId{++sCounter};
}

AndroidMediaCodecFrameId AndroidMediaCodecFrameId::GetNext() {
  static std::atomic<uint64_t> sCounter = 0;
  return AndroidMediaCodecFrameId{++sCounter};
}

std::ostream& operator<<(std::ostream& os, ScrollDirection aDirection) {
  switch (aDirection) {
    case ScrollDirection::eHorizontal:
      os << "horizontal";
      break;
    case ScrollDirection::eVertical:
      os << "vertical";
      break;
  }
  return os;
}

}  
}  
