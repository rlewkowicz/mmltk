/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsContentListDeclarations_h
#define nsContentListDeclarations_h

#include <stdint.h>

#include "nsCOMPtr.h"
#include "nsStringFwd.h"

class nsAtom;
class nsIContent;
class nsINode;

namespace mozilla::dom {
class CachableElementsByNameNodeList;
class ContentList;
class Element;
}  

#define kNameSpaceID_Wildcard INT32_MIN

using nsContentListMatchFunc = bool (*)(mozilla::dom::Element* aElement,
                                        int32_t aNamespaceID, nsAtom* aAtom,
                                        void* aData);

using nsContentListDestroyFunc = void (*)(void* aData);

using nsFuncStringContentListDataAllocator = void* (*)(nsINode * aRootNode,
                                                       const nsString* aString);

already_AddRefed<mozilla::dom::ContentList> NS_GetContentList(
    nsINode* aRootNode, int32_t aMatchNameSpaceId, const nsAString& aTagname);

template <class ListType>
already_AddRefed<mozilla::dom::ContentList> GetFuncStringContentList(
    nsINode* aRootNode, nsContentListMatchFunc aFunc,
    nsContentListDestroyFunc aDestroyFunc,
    nsFuncStringContentListDataAllocator aDataAllocator,
    const nsAString& aString);

#endif  // nsContentListDeclarations_h
