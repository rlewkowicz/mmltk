/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_BrowserDefines_h)
#define mozilla_BrowserDefines_h

#include "mozilla/CmdLineAndEnvUtils.h"

namespace mozilla {
namespace browser {
constexpr static const char* kRequiredArguments[] = {"url", "private-window"};
constexpr static auto kOptionalArguments = nullptr;
}  

template <typename CharT>
inline void EnsureBrowserCommandlineSafe(int aArgc, CharT** aArgv) {
  mozilla::EnsureCommandlineSafe(aArgc, aArgv, browser::kRequiredArguments,
                                 browser::kOptionalArguments);
}
}  

#endif
