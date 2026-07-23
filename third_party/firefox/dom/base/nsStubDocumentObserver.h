/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsStubDocumentObserver_h_
#define nsStubDocumentObserver_h_

#include "nsIDocumentObserver.h"

class nsStubDocumentObserver : public nsIDocumentObserver {
 public:
  NS_DECL_NSIDOCUMENTOBSERVER
};

#endif /* !defined(nsStubDocumentObserver_h_) */
