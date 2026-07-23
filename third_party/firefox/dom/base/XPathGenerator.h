/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef XPATHGENERATOR_H_
#define XPATHGENERATOR_H_
#include "nsINode.h"
#include "nsString.h"

class XPathGenerator {
 public:
  static void QuoteArgument(const nsAString& aArg, nsAString& aResult);

  static void EscapeName(const nsAString& aName, nsAString& aResult);

  static void Generate(const nsINode* aNode, nsAString& aResult);
};

#endif
