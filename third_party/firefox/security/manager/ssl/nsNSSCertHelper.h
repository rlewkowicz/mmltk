/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsNSSCertHelper_h
#define nsNSSCertHelper_h

#include "nsString.h"

void LossyUTF8ToUTF16(const char* str, uint32_t len,  nsAString& result);

nsresult GetPIPNSSBundleString(const char* stringName, nsAString& result);
nsresult GetPIPNSSBundleString(const char* stringName, nsACString& result);
nsresult PIPBundleFormatStringFromName(const char* stringName,
                                       const nsTArray<nsString>& params,
                                       nsAString& result);

#endif  // nsNSSCertHelper_h
