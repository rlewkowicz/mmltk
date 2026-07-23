/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef intl_components_Currency_h_
#define intl_components_Currency_h_

#include "mozilla/intl/ICU4CGlue.h"
#include "mozilla/intl/ICUError.h"
#include "mozilla/Result.h"

namespace mozilla::intl {

class Currency final {
 public:
  Currency() = delete;

  static Result<SpanEnumeration<char>, ICUError> GetISOCurrencies();
};

}  

#endif
