/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef DOM_SMIL_SMILCSSPROPERTY_H_
#define DOM_SMIL_SMILCSSPROPERTY_H_

#include "NonCustomCSSPropertyId.h"
#include "mozilla/SMILAttr.h"
#include "nsAtom.h"
#include "nsCSSValue.h"

namespace mozilla {
class ComputedStyle;
namespace dom {
class Element;
}  

class SMILCSSProperty : public SMILAttr {
 public:
  SMILCSSProperty(NonCustomCSSPropertyId aPropId, dom::Element* aElement,
                  const ComputedStyle* aBaseComputedStyle);

  nsresult ValueFromString(const nsAString& aStr,
                           const dom::SVGAnimationElement* aSrcElement,
                           SMILValue& aValue,
                           bool& aPreventCachingOfSandwich) const override;
  SMILValue GetBaseValue() const override;
  nsresult SetAnimValue(const SMILValue& aValue) override;
  void ClearAnimValue() override;

  static bool IsPropertyAnimatable(NonCustomCSSPropertyId aPropId);

 protected:
  NonCustomCSSPropertyId mPropId;
  dom::Element* mElement;

  const ComputedStyle* mBaseComputedStyle;
};

}  

#endif  // DOM_SMIL_SMILCSSPROPERTY_H_
