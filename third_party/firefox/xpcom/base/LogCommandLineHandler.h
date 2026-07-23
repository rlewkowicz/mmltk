/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LogCommandLineHandler_h
#define LogCommandLineHandler_h

#include <functional>
#include "nsString.h"

namespace mozilla {

void LoggingHandleCommandLineArgs(
    int argc, char const* const* argv,
    std::function<void(nsACString const&)> const& consumer);

}  

#endif
