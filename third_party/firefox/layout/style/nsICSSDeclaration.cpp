/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "nsICSSDeclaration.h"

#include "mozilla/css/Rule.h"
#include "nsINode.h"

using mozilla::dom::DocGroup;

DocGroup* nsICSSDeclaration::GetDocGroup() {
  nsINode* parentNode = GetAssociatedNode();
  if (!parentNode) {
    return nullptr;
  }

  return parentNode->GetDocGroup();
}

bool nsICSSDeclaration::IsReadOnly() {
  mozilla::css::Rule* rule = GetParentRule();
  return rule && rule->IsReadOnly();
}
