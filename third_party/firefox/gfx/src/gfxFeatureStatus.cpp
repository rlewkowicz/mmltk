/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gfxFeatureStatus.h"

#include "mozilla/Assertions.h"

namespace mozilla::gfx {

const char* FeatureStatusToString(FeatureStatus aStatus) {
  switch (aStatus) {
    case FeatureStatus::Unused:
      return "unused";
    case FeatureStatus::Unavailable:
      return "unavailable";
    case FeatureStatus::UnavailableInSafeMode:
      return "unavailable-in-safe-mode";
    case FeatureStatus::UnavailableNoGpuProcess:
      return "unavailable-no-gpu-process";
    case FeatureStatus::UnavailableNoHwCompositing:
      return "unavailable-no-hw-compositing";
    case FeatureStatus::UnavailableNoAngle:
      return "unavailable-no-angle";
    case FeatureStatus::Blocked:
      return "blocked";
    case FeatureStatus::BlockedNoGfxInfo:
      return "blocked-no-gfx-info";
    case FeatureStatus::Denied:
      return "denied";
    case FeatureStatus::Blocklisted:
      return "blocklisted";
    case FeatureStatus::Failed:
      return "failed";
    case FeatureStatus::Disabled:
      return "disabled";
    case FeatureStatus::Available:
      return "available";
    case FeatureStatus::ForceEnabled:
      return "force-enabled";
    case FeatureStatus::Broken:
      return "broken";
    default:
      MOZ_ASSERT_UNREACHABLE("missing graphics feature status case");
      return "unknown";
  }
}

bool IsFeatureStatusFailure(FeatureStatus aStatus) {
  return aStatus != FeatureStatus::Unused &&
         aStatus != FeatureStatus::Available &&
         aStatus != FeatureStatus::ForceEnabled;
}

bool IsFeatureStatusSuccess(FeatureStatus aStatus) {
  return aStatus == FeatureStatus::Available ||
         aStatus == FeatureStatus::ForceEnabled;
}

}  
