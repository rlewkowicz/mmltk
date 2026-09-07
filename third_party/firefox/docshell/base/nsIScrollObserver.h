/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsIScrollObserver_h_
#define nsIScrollObserver_h_

#include "nsISupports.h"
#include "Units.h"

#define NS_ISCROLLOBSERVER_IID \
  {0xaa5026eb, 0x2f88, 0x4026, {0xa4, 0x6b, 0xf4, 0x59, 0x6b, 0x4e, 0xdf, 0x00}}

class nsIScrollObserver : public nsISupports {
 public:
  NS_INLINE_DECL_STATIC_IID(NS_ISCROLLOBSERVER_IID)

  virtual void ScrollPositionChanged() = 0;

  MOZ_CAN_RUN_SCRIPT virtual void AsyncPanZoomStarted() {};

  MOZ_CAN_RUN_SCRIPT virtual void AsyncPanZoomStopped() {};
};

#endif /* nsIScrollObserver_h_ */
