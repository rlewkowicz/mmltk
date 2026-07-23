/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsUConvPropertySearch_h_
#define nsUConvPropertySearch_h_

#include "nsStringFwd.h"

struct nsUConvProp {
  const char* const mKey;
  const char* const mValue;
  const uint32_t mValueLength;
};

class nsUConvPropertySearch {
 public:
  static nsresult SearchPropertyValue(const nsUConvProp aProperties[],
                                      int32_t aNumberOfProperties,
                                      const nsACString& aKey,
                                      nsACString& aValue);
};

#endif /* nsUConvPropertySearch_h_ */
