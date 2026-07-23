/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_CookiePrefixes_h
#define mozilla_net_CookiePrefixes_h

#include "mozilla/net/NeckoChannelParams.h"

namespace mozilla::net {

class CookiePrefixes final {
 public:
  enum Prefix {
    eSecure,
    eHttp,
    eHost,
    eHostHttp,
  };

  static bool Has(Prefix aPrefix, const nsAString& aString);

  static bool Has(const nsACString& aString);

  static bool Check(const CookieStruct& aCookieData, bool aSecureRequest);
};

}  

#endif  // mozilla_net_CookiePrefixes_h
