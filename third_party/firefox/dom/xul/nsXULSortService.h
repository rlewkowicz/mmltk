/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsXULSortService_h
#define nsXULSortService_h

#include "nsAString.h"
#include "nsError.h"

namespace mozilla {

namespace dom {
class Element;
}  

nsresult XULWidgetSort(dom::Element* aNode, const nsAString& aSortKey,
                       const nsAString& aSortHints);

}  

#endif  // nsXULSortService_h
