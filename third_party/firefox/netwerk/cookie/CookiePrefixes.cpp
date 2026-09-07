/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CookiePrefixes.h"

namespace mozilla::net {

namespace {

struct CookiePrefix {
  CookiePrefixes::Prefix mPrefix;
  nsCString mPrefixCString;
  nsString mPrefixString;
  std::function<bool(const CookieStruct&, bool)> mCallback;
};

MOZ_RUNINIT CookiePrefix gCookiePrefixes[] = {
    {CookiePrefixes::eHostHttp, "__Host-Http-"_ns, u"__Host-Http-"_ns,
     [](const CookieStruct& aCookieData, bool aSecureRequest) -> bool {
       return aSecureRequest && aCookieData.isSecure() &&
              aCookieData.isHttpOnly() && aCookieData.host()[0] != '.' &&
              aCookieData.path().EqualsLiteral("/");
     }},

    {CookiePrefixes::eHost, "__Host-"_ns, u"__Host-"_ns,
     [](const CookieStruct& aCookieData, bool aSecureRequest) -> bool {
       return aSecureRequest && aCookieData.isSecure() &&
              aCookieData.host()[0] != '.' &&
              aCookieData.path().EqualsLiteral("/");
     }},

    {CookiePrefixes::eHttp, "__Http-"_ns, u"__Http-"_ns,
     [](const CookieStruct& aCookieData, bool aSecureRequest) -> bool {
       return aSecureRequest && aCookieData.isSecure() &&
              aCookieData.isHttpOnly();
     }},

    {CookiePrefixes::eSecure, "__Secure-"_ns, u"__Secure-"_ns,
     [](const CookieStruct& aCookieData, bool aSecureRequest) -> bool {
       return aSecureRequest && aCookieData.isSecure();
     }},
};

}  

bool CookiePrefixes::Has(Prefix aPrefix, const nsAString& aString) {
  for (CookiePrefix& prefix : gCookiePrefixes) {
    if (prefix.mPrefix == aPrefix) {
      return StringBeginsWith(aString, prefix.mPrefixString,
                              nsCaseInsensitiveStringComparator);
    }
  }

  return false;
}

bool CookiePrefixes::Has(const nsACString& aString) {
  for (CookiePrefix& prefix : gCookiePrefixes) {
    if (StringBeginsWith(aString, prefix.mPrefixCString,
                         nsCaseInsensitiveCStringComparator)) {
      return true;
    }
  }

  return false;
}

bool CookiePrefixes::Check(const CookieStruct& aCookieData,
                           bool aSecureRequest) {
  for (CookiePrefix& prefix : gCookiePrefixes) {
    if (StringBeginsWith(aCookieData.name(), prefix.mPrefixCString,
                         nsCaseInsensitiveCStringComparator)) {
      return prefix.mCallback(aCookieData, aSecureRequest);
    }
  }

  return true;
}

}  
