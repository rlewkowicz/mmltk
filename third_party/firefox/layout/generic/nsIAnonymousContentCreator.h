/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsIAnonymousContentCreator_h_
#define nsIAnonymousContentCreator_h_

#include "mozilla/AnonymousContentKey.h"
#include "nsQueryFrame.h"
#include "nsTArrayForwardDeclare.h"

class nsIContent;

class nsIAnonymousContentCreator {
 public:
  NS_DECL_QUERYFRAME_TARGET(nsIAnonymousContentCreator)

  struct ContentInfo {
    explicit ContentInfo(
        nsIContent* aContent,
        mozilla::AnonymousContentKey aKey = mozilla::AnonymousContentKey::None)
        : mContent(aContent), mKey(aKey) {}

    nsIContent* mContent;
    mozilla::AnonymousContentKey mKey;
  };

  virtual nsresult CreateAnonymousContent(nsTArray<ContentInfo>& aElements) = 0;

  virtual void AppendAnonymousContentTo(nsTArray<nsIContent*>& aElements,
                                        uint32_t aFilter) = 0;
};

#endif
