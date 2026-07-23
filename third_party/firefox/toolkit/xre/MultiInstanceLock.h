/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(MULTIINSTANCELOCK_H)
#define MULTIINSTANCELOCK_H

#include "nsIFile.h"



namespace mozilla {

using MultiInstLockHandle = int;
#  define MULTI_INSTANCE_LOCK_HANDLE_ERROR -1

MultiInstLockHandle OpenMultiInstanceLock(const char* nameToken,
                                          const char16_t* installPath);

void ReleaseMultiInstanceLock(MultiInstLockHandle lock);

bool IsOtherInstanceRunning(MultiInstLockHandle lock, bool* aResult);

already_AddRefed<nsIFile> GetNormalizedAppFile(nsIFile* aAppFile);

bool GetMultiInstanceLockFileName(const char* nameToken,
                                  const char16_t* installPath,
                                  nsCString& filePath);

};  

#endif
