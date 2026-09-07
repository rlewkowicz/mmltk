/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DocumentStyleRootIterator_h
#define DocumentStyleRootIterator_h

#include "nsTArray.h"

class nsIContent;

class nsINode;

namespace mozilla {

namespace dom {
class Element;
}  

class DocumentStyleRootIterator {
 public:
  explicit DocumentStyleRootIterator(nsINode* aStyleRoot);
  MOZ_COUNTED_DTOR(DocumentStyleRootIterator)

  dom::Element* GetNextStyleRoot();

 private:
  AutoTArray<nsIContent*, 8> mStyleRoots;
  uint32_t mPosition;
};

}  

#endif  // DocumentStyleRootIterator_h
