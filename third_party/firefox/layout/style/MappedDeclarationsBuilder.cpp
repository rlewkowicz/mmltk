/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MappedDeclarationsBuilder.h"

#include "mozilla/dom/Document.h"
#include "nsAttrValue.h"
#include "nsAttrValueInlines.h"
#include "nsPresContext.h"

namespace mozilla {

void MappedDeclarationsBuilder::SetIdentAtomValue(NonCustomCSSPropertyId aId,
                                                  nsAtom* aValue) {
  Servo_DeclarationBlock_SetIdentStringValue(&EnsureDecls(), aId, aValue);
}

void MappedDeclarationsBuilder::SetBackgroundImage(const nsAttrValue& aValue) {
  if (aValue.Type() != nsAttrValue::eURL) {
    return;
  }
  nsAutoCString utf8;
  if (nsIURI* uri = aValue.GetURLValue()) {
    uri->GetSpec(utf8);
  } else {
    nsAutoString str;
    aValue.ToString(str);
    CopyUTF16toUTF8(str, utf8);
  }
  Servo_DeclarationBlock_SetBackgroundImage(
      &EnsureDecls(), &utf8, mDocument.DefaultStyleAttrURLData());
}

}  
