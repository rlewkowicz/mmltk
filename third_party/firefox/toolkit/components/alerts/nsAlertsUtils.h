/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsAlertsUtils_h
#define nsAlertsUtils_h

#include "nsIPrincipal.h"
#include "nsString.h"

class nsAlertsUtils final {
 public:
  nsAlertsUtils() = delete;
  static bool IsActionablePrincipal(nsIPrincipal* aPrincipal);

  static void GetSourceHostPort(nsIPrincipal* aPrincipal, nsAString& aHostPort);

  static nsresult GetOrigin(nsIPrincipal* aPrincipal, nsACString& aOrigin);
};
#endif /* nsAlertsUtils_h */
