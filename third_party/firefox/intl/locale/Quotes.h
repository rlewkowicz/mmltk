/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_intl_Quotes_h_
#define mozilla_intl_Quotes_h_

#include "nsAtom.h"

namespace mozilla {
namespace intl {

struct Quotes {
  char16_t mChars[4];
};

const Quotes* QuotesForLang(const nsAtom* aLang);

}  
}  

#endif  // mozilla_intl_Quotes_h_
