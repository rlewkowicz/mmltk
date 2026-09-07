/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef _nsIStatefulFrame_h
#define _nsIStatefulFrame_h

#include "nsContentUtils.h"
#include "nsQueryFrame.h"

namespace mozilla {
class PresState;
}  

class nsIStatefulFrame {
 public:
  NS_DECL_QUERYFRAME_TARGET(nsIStatefulFrame)

  virtual mozilla::UniquePtr<mozilla::PresState> SaveState() = 0;

  NS_IMETHOD RestoreState(mozilla::PresState* aState) = 0;

  virtual void GenerateStateKey(nsIContent* aContent,
                                mozilla::dom::Document* aDocument,
                                nsACString& aKey) {
    nsContentUtils::GenerateStateKey(aContent, aDocument, aKey);
  };
};

#endif /* _nsIStatefulFrame_h */
