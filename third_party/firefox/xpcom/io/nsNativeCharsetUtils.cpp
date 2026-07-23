/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#  include "nsAString.h"
#  include "nsReadableUtils.h"
#  include "nsString.h"

nsresult NS_CopyNativeToUnicode(const nsACString& aInput, nsAString& aOutput) {
  CopyUTF8toUTF16(aInput, aOutput);
  return NS_OK;
}

nsresult NS_CopyUnicodeToNative(const nsAString& aInput, nsACString& aOutput) {
  CopyUTF16toUTF8(aInput, aOutput);
  return NS_OK;
}

