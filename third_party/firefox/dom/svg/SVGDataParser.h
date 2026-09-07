/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_SVG_SVGDATAPARSER_H_
#define DOM_SVG_SVGDATAPARSER_H_

#include "nsAString.h"

namespace mozilla {

class SVGDataParser {
 public:
  explicit SVGDataParser(const nsAString& aValue);

 protected:
  bool SkipCommaWsp();

  bool SkipWsp();

  nsAString::const_iterator mIter;
  nsAString::const_iterator mEnd;
};

}  

#endif  // DOM_SVG_SVGDATAPARSER_H_
