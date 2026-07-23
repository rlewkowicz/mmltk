/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _nsContentPolicy_h_
#define _nsContentPolicy_h_

#include "nsCategoryCache.h"
#include "nsIContentPolicy.h"


class nsContentPolicy : public nsIContentPolicy {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSICONTENTPOLICY

  nsContentPolicy();

 protected:
  virtual ~nsContentPolicy();

 private:
  nsCategoryCache<nsIContentPolicy> mPolicies;

  using CPMethod = decltype(&nsIContentPolicy::ShouldProcess);

  nsresult CheckPolicy(CPMethod policyMethod, nsIURI* aURI,
                       nsILoadInfo* aLoadInfo, int16_t* decision);
};

nsresult NS_NewContentPolicy(nsIContentPolicy** aResult);

#endif /* _nsContentPolicy_h_ */
