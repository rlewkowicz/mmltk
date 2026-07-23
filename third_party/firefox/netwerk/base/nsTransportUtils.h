/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsTransportUtils_h_
#define nsTransportUtils_h_

#include "nsITransport.h"

nsresult net_NewTransportEventSinkProxy(nsITransportEventSink** aResult,
                                        nsITransportEventSink* aSink,
                                        nsIEventTarget* aTarget);

#endif  // nsTransportUtils_h_
