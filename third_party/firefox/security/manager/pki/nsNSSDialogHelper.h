/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsNSSDialogHelper_h
#define nsNSSDialogHelper_h

#include "nsError.h"

class mozIDOMWindowProxy;
class nsISupports;

class nsNSSDialogHelper {
 public:
  static nsresult openDialog(mozIDOMWindowProxy* window, const char* url,
                             nsISupports* params, bool modal = true);
};

#endif  // nsNSSDialogHelper_h
