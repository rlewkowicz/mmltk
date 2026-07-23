/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(nsNativeCharsetUtils_h_)
#define nsNativeCharsetUtils_h_


#include "nsError.h"
#include "nsStringFwd.h"

nsresult NS_CopyNativeToUnicode(const nsACString& aInput, nsAString& aOutput);
nsresult NS_CopyUnicodeToNative(const nsAString& aInput, nsACString& aOutput);

inline constexpr bool NS_IsNativeUTF8() {
  return true;
}

#endif
