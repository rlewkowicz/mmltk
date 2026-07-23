/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef DOM_SVG_SVGMOTIONSMILATTR_H_
#define DOM_SVG_SVGMOTIONSMILATTR_H_

#include "mozilla/SMILAttr.h"

class nsIContent;

namespace mozilla {

class SMILValue;

namespace dom {
class SVGAnimationElement;
class SVGElement;
}  

class SVGMotionSMILAttr : public SMILAttr {
 public:
  explicit SVGMotionSMILAttr(dom::SVGElement* aSVGElement)
      : mSVGElement(aSVGElement) {}

  nsresult ValueFromString(const nsAString& aStr,
                           const dom::SVGAnimationElement* aSrcElement,
                           SMILValue& aValue,
                           bool& aPreventCachingOfSandwich) const override;
  SMILValue GetBaseValue() const override;
  nsresult SetAnimValue(const SMILValue& aValue) override;
  void ClearAnimValue() override;
  const nsIContent* GetTargetNode() const override;

 protected:
  dom::SVGElement* mSVGElement;
};

}  

#endif  // DOM_SVG_SVGMOTIONSMILATTR_H_
