/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_SMIL_SMILATTR_H_
#define DOM_SMIL_SMILATTR_H_

#include "nsStringFwd.h"
#include "nscore.h"

class nsIContent;

namespace mozilla {

class SMILValue;

namespace dom {
class SVGAnimationElement;
}  


class SMILAttr {
 public:
  virtual nsresult ValueFromString(
      const nsAString& aStr,
      const mozilla::dom::SVGAnimationElement* aSrcElement, SMILValue& aValue,
      bool& aPreventCachingOfSandwich) const = 0;

  virtual SMILValue GetBaseValue() const = 0;

  virtual void ClearAnimValue() = 0;

  virtual nsresult SetAnimValue(const SMILValue& aValue) = 0;

  virtual const nsIContent* GetTargetNode() const { return nullptr; }

  virtual ~SMILAttr() = default;
};

}  

#endif  // DOM_SMIL_SMILATTR_H_
