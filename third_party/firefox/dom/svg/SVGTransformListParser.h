/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_SVG_SVGTRANSFORMLISTPARSER_H_
#define DOM_SVG_SVGTRANSFORMLISTPARSER_H_

#include "SVGDataParser.h"
#include "nsTArray.h"


namespace mozilla {

class SVGTransform;

class SVGTransformListParser : public SVGDataParser {
 public:
  explicit SVGTransformListParser(const nsAString& aValue)
      : SVGDataParser(aValue) {}

  bool Parse();

  const nsTArray<SVGTransform>& GetTransformList() const { return mTransforms; }

 private:
  bool ParseArguments(float* aResult, uint32_t aMaxCount,
                      uint32_t* aParsedCount);

  bool ParseTransforms();

  bool ParseTransform();

  bool ParseTranslate();
  bool ParseScale();
  bool ParseRotate();
  bool ParseSkewX();
  bool ParseSkewY();
  bool ParseMatrix();

  FallibleTArray<SVGTransform> mTransforms;
};

}  

#endif  // DOM_SVG_SVGTRANSFORMLISTPARSER_H_
