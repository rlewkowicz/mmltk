/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsInterfaceRequestorAgg_h_
#define nsInterfaceRequestorAgg_h_

#include "nsError.h"

class nsIEventTarget;
class nsIInterfaceRequestor;

extern nsresult NS_NewInterfaceRequestorAggregation(
    nsIInterfaceRequestor* aFirst, nsIInterfaceRequestor* aSecond,
    nsIInterfaceRequestor** aResult);

extern nsresult NS_NewInterfaceRequestorAggregation(
    nsIInterfaceRequestor* aFirst, nsIInterfaceRequestor* aSecond,
    nsIEventTarget* aTarget, nsIInterfaceRequestor** aResult);

#endif  // !defined( nsInterfaceRequestorAgg_h_ )
