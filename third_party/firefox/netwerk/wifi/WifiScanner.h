/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#pragma once

#include "nsTArray.h"
#include "mozilla/RefPtr.h"

class nsIWifiAccessPoint;

namespace mozilla {

class WifiScanner {
 public:
  virtual nsresult GetAccessPointsFromWLAN(
      nsTArray<RefPtr<nsIWifiAccessPoint>>& accessPoints) = 0;

  virtual ~WifiScanner() = default;
};

}  
