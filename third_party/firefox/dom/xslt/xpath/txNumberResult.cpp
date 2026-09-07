/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "txExprResult.h"


NumberResult::NumberResult(double aValue, txResultRecycler* aRecycler)
    : txAExprResult(aRecycler), value(aValue) {}  


short NumberResult::getResultType() {
  return txAExprResult::NUMBER;
}  

void NumberResult::stringValue(nsString& aResult) {
  txDouble::toString(value, aResult);
}

const nsString* NumberResult::stringValuePointer() { return nullptr; }

bool NumberResult::booleanValue() {
  return (bool)(value != 0.0 && !std::isnan(value));
}  

double NumberResult::numberValue() { return this->value; }  
