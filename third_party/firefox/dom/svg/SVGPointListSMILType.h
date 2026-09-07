/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_SVG_SVGPOINTLISTSMILTYPE_H_
#define DOM_SVG_SVGPOINTLISTSMILTYPE_H_

#include "mozilla/SMILType.h"

namespace mozilla {

class SMILValue;

class SVGPointListSMILType : public SMILType {
 public:
  static SVGPointListSMILType sSingleton;

 protected:

  void InitValue(SMILValue& aValue) const override;

  void DestroyValue(SMILValue& aValue) const override;
  nsresult Assign(SMILValue& aDest, const SMILValue& aSrc) const override;
  bool IsEqual(const SMILValue& aLeft, const SMILValue& aRight) const override;
  nsresult Add(SMILValue& aDest, const SMILValue& aValueToAdd,
               uint32_t aCount) const override;
  nsresult ComputeDistance(const SMILValue& aFrom, const SMILValue& aTo,
                           double& aDistance) const override;
  nsresult Interpolate(const SMILValue& aStartVal, const SMILValue& aEndVal,
                       double aUnitDistance, SMILValue& aResult) const override;

 private:
  constexpr SVGPointListSMILType() = default;
};

}  

#endif  // DOM_SVG_SVGPOINTLISTSMILTYPE_H_
