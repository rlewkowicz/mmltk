/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsArrayEnumerator_h_
#define nsArrayEnumerator_h_


#include "nsISupports.h"

class nsISimpleEnumerator;
class nsIArray;
class nsCOMArray_base;

nsresult NS_NewArrayEnumerator(nsISimpleEnumerator** aResult, nsIArray* aArray,
                               const nsID& aEntryIID = NS_GET_IID(nsISupports));

nsresult NS_NewArrayEnumerator(nsISimpleEnumerator** aResult,
                               const nsCOMArray_base& aArray,
                               const nsID& aEntryIID = NS_GET_IID(nsISupports));

#endif
