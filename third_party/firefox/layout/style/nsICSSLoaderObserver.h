/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsICSSLoaderObserver_h_
#define nsICSSLoaderObserver_h_

#include "nsISupports.h"

#define NS_ICSSLOADEROBSERVER_IID \
  {0xf51fbf2c, 0xfe4b, 0x4a15, {0xaf, 0x7e, 0x5e, 0x20, 0x64, 0x5f, 0xaf, 0x58}}

namespace mozilla {
class StyleSheet;
}

class nsICSSLoaderObserver : public nsISupports {
 public:
  NS_INLINE_DECL_STATIC_IID(NS_ICSSLOADEROBSERVER_IID)

  NS_IMETHOD StyleSheetLoaded(mozilla::StyleSheet* aSheet, bool aWasDeferred,
                              nsresult aStatus) = 0;
};

#endif  // nsICSSLoaderObserver_h_
