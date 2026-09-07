/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(TOOLKIT_COMPONENTS_REMOTE_REMOTEUTILS_H_)
#define TOOLKIT_COMPONENTS_REMOTE_REMOTEUTILS_H_

#include "mozilla/HashFunctions.h"

#include "nsHashKeys.h"
#include "nsString.h"


mozilla::UniquePtr<char[]> ConstructCommandLine(int32_t argc, const char** argv,
                                                const char* aStartupToken,
                                                int* aCommandLineLength);

#endif
