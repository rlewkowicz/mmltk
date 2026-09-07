/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SanitizationUtils.h"

#include "mozilla/dom/quota/QuotaManager.h"
#include "nsString.h"

namespace mozilla::dom::quota {

nsAutoCString MakeSanitizedOriginCString(const nsACString& aOrigin) {

  nsAutoCString res{aOrigin};

  res.ReplaceChar(QuotaManager::kReplaceChars, '+');

  return res;
}

nsAutoString MakeSanitizedOriginString(const nsACString& aOrigin) {
  return NS_ConvertASCIItoUTF16(MakeSanitizedOriginCString(aOrigin));
}

}  
