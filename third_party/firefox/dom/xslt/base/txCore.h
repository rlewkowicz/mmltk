/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _txCore_h_
#define _txCore_h_

#include "nsDebug.h"
#include "nsISupportsImpl.h"
#include "nsStringFwd.h"
#include "nscore.h"

class txObject {
 public:
  MOZ_COUNTED_DEFAULT_CTOR(txObject)

  MOZ_COUNTED_DTOR_VIRTUAL(txObject)
};

class txDouble {
 public:
  static void toString(double aValue, nsAString& aDest);

  static double toDouble(const nsAString& aStr);
};

#endif
