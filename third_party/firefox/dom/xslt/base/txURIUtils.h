/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TRANSFRMX_URIUTILS_H
#define TRANSFRMX_URIUTILS_H

#include "txCore.h"

class nsINode;

namespace mozilla::dom {
class Document;
}  


class URIUtils {
 public:
  static void ResetWithSource(mozilla::dom::Document* aNewDoc,
                              nsINode* aSourceNode);

  static void resolveHref(const nsAString& href, const nsAString& base,
                          nsAString& dest);
};  

#endif
