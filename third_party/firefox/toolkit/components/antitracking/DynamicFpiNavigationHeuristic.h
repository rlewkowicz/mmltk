/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dynamicfpinavigationheuristic_h
#define mozilla_dynamicfpinavigationheuristic_h

#include "nsIEffectiveTLDService.h"
#include "nsIPrincipal.h"
#include "nsIWebProgressListener.h"
#include "nsWeakReference.h"

namespace mozilla {

namespace dom {
class BrowsingContextWebProgress;
class CanonicalBrowsingContext;
}  

class DynamicFpiNavigationHeuristic {
 public:
  static void MaybeGrantStorageAccess(
      dom::CanonicalBrowsingContext* aBrowsingContext, nsIChannel* aChannel);
};

}  

#endif  // mozilla_dynamicfpinavigationheuristic_h
