/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "txExprResult.h"

BooleanResult::BooleanResult(bool boolean) : txAExprResult(nullptr) {
  this->value = boolean;
}  


short BooleanResult::getResultType() {
  return txAExprResult::BOOLEAN;
}  

void BooleanResult::stringValue(nsString& aResult) {
  if (value) {
    aResult.AppendLiteral("true");
  } else {
    aResult.AppendLiteral("false");
  }
}

const nsString* BooleanResult::stringValuePointer() {
  return nullptr;
}

bool BooleanResult::booleanValue() { return this->value; }  

double BooleanResult::numberValue() {
  return (value) ? 1.0 : 0.0;
}  
