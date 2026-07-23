/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef INTL_UNICHARUTIL_UTIL_NSSPECIALCASINGDATA_H_
#define INTL_UNICHARUTIL_UTIL_NSSPECIALCASINGDATA_H_

#include <stdint.h>

namespace mozilla {
namespace unicode {

struct MultiCharMapping {
  char16_t mOriginalChar;
  char16_t mMappedChars[3];
};

const MultiCharMapping* SpecialUpper(uint32_t aCh);
const MultiCharMapping* SpecialLower(uint32_t aCh);
const MultiCharMapping* SpecialTitle(uint32_t aCh);

}  
}  

#endif  // INTL_UNICHARUTIL_UTIL_NSSPECIALCASINGDATA_H_
