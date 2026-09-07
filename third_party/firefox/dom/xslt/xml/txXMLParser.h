/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MITRE_XMLPARSER_H
#define MITRE_XMLPARSER_H

#include "txCore.h"

class txXPathNode;

namespace mozilla {
template <typename V, typename E>
class Result;
}  


mozilla::Result<txXPathNode, nsresult> txParseDocumentFromURI(
    const nsAString& aHref, const txXPathNode& aLoader, nsAString& aErrMsg);

#endif
