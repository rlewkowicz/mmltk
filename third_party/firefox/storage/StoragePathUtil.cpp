/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/storage/StoragePathUtil.h"

#include <cctype>

#include "nsEscape.h"
#include "nsString.h"

namespace mozilla::storage {

void PreparePathForURI(nsACString& aPath) {
  nsAutoCString escaped;
  if (NS_EscapeURLSpan(aPath, esc_FilePath | esc_Forced, escaped)) {
    aPath = escaped;
  }
}

}  
