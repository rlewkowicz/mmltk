/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef gfx_src_gfxFeatureStatus_h_
#define gfx_src_gfxFeatureStatus_h_

namespace mozilla::gfx {

enum class FeatureStatus {
  Unused,
  Unavailable,
  UnavailableInSafeMode,
  UnavailableNoGpuProcess,
  UnavailableNoHwCompositing,
  UnavailableNoAngle,
  Blocked,
  BlockedNoGfxInfo,
  Denied,
  Blocklisted,
  Failed,
  Disabled,
  Available,
  ForceEnabled,
  Broken,
  LAST
};

const char* FeatureStatusToString(FeatureStatus aStatus);
bool IsFeatureStatusFailure(FeatureStatus aStatus);
bool IsFeatureStatusSuccess(FeatureStatus aStatus);

}  

#endif  // gfx_src_gfxFeatureStatus_h_
