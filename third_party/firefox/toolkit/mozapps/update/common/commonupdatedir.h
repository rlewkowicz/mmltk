/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(COMMONUPDATEDIR_H)
#define COMMONUPDATEDIR_H

#include "mozilla/UniquePtr.h"

typedef char NS_tchar;

bool GetInstallHash(const char16_t* installPath,
                    mozilla::UniquePtr<NS_tchar[]>& result);


#endif
