/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RemoteDecodeUtils.h"

#include "mozilla/ipc/UtilityProcessChild.h"

namespace mozilla {

using UtilityProcessKind = ipc::UtilityProcessKind;

UtilityProcessKind GetCurrentUtilityProcessKind() {
  MOZ_ASSERT(XRE_IsUtilityProcess());
  return ipc::UtilityProcessChild::GetSingleton()->mKind;
}

UtilityProcessKind GetUtilityProcessKindFromLocation(RemoteMediaIn aLocation) {
  switch (aLocation) {
    case RemoteMediaIn::UtilityProcess_Generic:
      return UtilityProcessKind::GENERIC_UTILITY;
    default:
      MOZ_ASSERT_UNREACHABLE("Unsupported RemoteMediaIn");
      return UtilityProcessKind::COUNT;
  }
}

RemoteMediaIn GetRemoteMediaInFromKind(UtilityProcessKind aKind) {
  switch (aKind) {
    case UtilityProcessKind::GENERIC_UTILITY:
      return RemoteMediaIn::UtilityProcess_Generic;
    default:
      MOZ_ASSERT_UNREACHABLE("Unsupported UtilityProcessKind");
      return RemoteMediaIn::Unspecified;
  }
}

RemoteMediaIn GetRemoteMediaInFromVideoBridgeSource(
    layers::VideoBridgeSource aSource) {
  switch (aSource) {
    case layers::VideoBridgeSource::RddProcess:
      return RemoteMediaIn::RddProcess;
    case layers::VideoBridgeSource::GpuProcess:
      return RemoteMediaIn::GpuProcess;
    default:
      MOZ_ASSERT_UNREACHABLE("Unsupported VideoBridgeSource");
      return RemoteMediaIn::Unspecified;
  }
}

layers::VideoBridgeSource GetVideoBridgeSourceFromRemoteMediaIn(
    RemoteMediaIn aSource) {
  switch (aSource) {
    case RemoteMediaIn::RddProcess:
      return layers::VideoBridgeSource::RddProcess;
    case RemoteMediaIn::GpuProcess:
      return layers::VideoBridgeSource::GpuProcess;
    default:
      MOZ_ASSERT_UNREACHABLE("Unsupported RemoteMediaIn");
      return layers::VideoBridgeSource::_Count;
  }
}

const char* RemoteMediaInToStr(RemoteMediaIn aLocation) {
  switch (aLocation) {
    case RemoteMediaIn::RddProcess:
      return "RDD";
    case RemoteMediaIn::GpuProcess:
      return "GPU";
    case RemoteMediaIn::UtilityProcess_Generic:
      return "Utility Generic";
    default:
      MOZ_ASSERT_UNREACHABLE("Unsupported RemoteMediaIn");
      return "Unknown";
  }
}

}  
