/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(mozilla_SharedLibrary_h)
#define mozilla_SharedLibrary_h

#if defined(MOZILLA_INTERNAL_API)

#  include "prlink.h"
#  include "mozilla/Char16.h"

namespace mozilla {

inline PRLibrary*
LoadLibraryWithFlags(const char* aPath, PRUint32 aFlags = 0)
{
  PRLibSpec libSpec;
  libSpec.type = PR_LibSpec_Pathname;
  libSpec.value.pathname = aPath;
  return PR_LoadLibraryWithFlags(libSpec, aFlags);
}

} 

#endif

#endif
