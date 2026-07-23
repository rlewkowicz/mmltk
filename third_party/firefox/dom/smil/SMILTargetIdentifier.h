/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_SMIL_SMILTARGETIDENTIFIER_H_
#define DOM_SMIL_SMILTARGETIDENTIFIER_H_

#include "mozilla/dom/Element.h"
#include "nsAtom.h"
#include "nsIContent.h"

class nsIContent;

namespace mozilla {
namespace dom {
class Element;
}


struct SMILTargetIdentifier {
  SMILTargetIdentifier()
      : mElement(nullptr),
        mAttributeName(nullptr),
        mAttributeNamespaceID(kNameSpaceID_Unknown) {}

  inline bool Equals(const SMILTargetIdentifier& aOther) const {
    return (aOther.mElement == mElement &&
            aOther.mAttributeName == mAttributeName &&
            aOther.mAttributeNamespaceID == mAttributeNamespaceID);
  }

  RefPtr<mozilla::dom::Element> mElement;
  RefPtr<nsAtom> mAttributeName;
  int32_t mAttributeNamespaceID;
};

class SMILWeakTargetIdentifier {
 public:
  SMILWeakTargetIdentifier() : mElement(nullptr), mAttributeName(nullptr) {}

  SMILWeakTargetIdentifier& operator=(const SMILTargetIdentifier& aOther) {
    mElement = aOther.mElement;
    mAttributeName = aOther.mAttributeName;
    return *this;
  }

  inline bool Equals(const SMILTargetIdentifier& aOther) const {
    return (aOther.mElement == mElement &&
            aOther.mAttributeName == mAttributeName);
  }

 private:
  const nsIContent* mElement;
  const nsAtom* mAttributeName;
};

}  

#endif  // DOM_SMIL_SMILTARGETIDENTIFIER_H_
