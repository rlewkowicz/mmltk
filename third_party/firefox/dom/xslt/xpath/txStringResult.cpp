/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "txExprResult.h"

StringResult::StringResult(txResultRecycler* aRecycler)
    : txAExprResult(aRecycler) {}

StringResult::StringResult(const nsAString& aValue, txResultRecycler* aRecycler)
    : txAExprResult(aRecycler), mValue(aValue) {}


short StringResult::getResultType() {
  return txAExprResult::STRING;
}  

void StringResult::stringValue(nsString& aResult) { aResult.Append(mValue); }

const nsString* StringResult::stringValuePointer() { return &mValue; }

bool StringResult::booleanValue() {
  return !mValue.IsEmpty();
}  

double StringResult::numberValue() {
  return txDouble::toDouble(mValue);
}  
