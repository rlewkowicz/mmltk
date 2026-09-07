/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ConstraintValidition_h_
#define mozilla_dom_ConstraintValidition_h_

#include "nsIConstraintValidation.h"
#include "nsString.h"

namespace mozilla {
class ErrorResult;

namespace dom {

class ConstraintValidation : public nsIConstraintValidation {
 public:
  void GetValidationMessage(nsAString& aValidationMessage,
                            mozilla::ErrorResult& aError);
  bool CheckValidity();

 protected:
  ConstraintValidation();

  virtual ~ConstraintValidation() = default;

  void SetCustomValidity(const nsAString& aError);

  virtual nsresult GetValidationMessage(nsAString& aValidationMessage,
                                        ValidityStateType aType) {
    return NS_OK;
  }

 private:
  nsString mCustomValidity;
};

}  
}  

#endif  // mozilla_dom_ConstraintValidition_h_
