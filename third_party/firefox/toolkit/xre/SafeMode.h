/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_SafeMode_h)
#define mozilla_SafeMode_h


#include "mozilla/CmdLineAndEnvUtils.h"
#include "mozilla/Maybe.h"


#undef None

namespace mozilla {

enum class SafeModeFlag : uint32_t {
  None = 0,
  Unset = (1 << 0),
  NoKeyPressCheck = (1 << 1),
};

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(SafeModeFlag)

template <typename CharT>
inline Maybe<bool> IsSafeModeRequested(
    int& aArgc, CharT* aArgv[],
    const SafeModeFlag aFlags = SafeModeFlag::Unset) {
  CheckArgFlag checkArgFlags = CheckArgFlag::None;
  if (aFlags & SafeModeFlag::Unset) {
    checkArgFlags |= CheckArgFlag::RemoveArg;
  }

  ArgResult ar = CheckArg(aArgc, aArgv, "safe-mode", nullptr, checkArgFlags);
  if (ar == ARG_BAD) {
    return Nothing();
  }

  bool result = ar == ARG_FOUND;



  if (EnvHasValue("MOZ_SAFE_MODE_RESTART")) {
    result = true;
    if (aFlags & SafeModeFlag::Unset) {
      SaveToEnv("MOZ_SAFE_MODE_RESTART=");
    }
  }

  return Some(result);
}

}  

#endif
