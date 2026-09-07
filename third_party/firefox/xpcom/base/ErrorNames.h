/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ErrorNames_h
#define mozilla_ErrorNames_h

#include "nsError.h"
#include "nsStringFwd.h"

namespace mozilla {

void GetErrorName(nsresult rv, nsACString& name);

const char* GetStaticErrorName(nsresult rv);

}  

nsCString format_as(nsresult rv);

#endif  // mozilla_ErrorNames_h
