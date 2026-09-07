/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_NoVarySearchUtils_h
#define mozilla_net_NoVarySearchUtils_h

#include "nsString.h"
#include "nsTArray.h"

class nsIURI;

namespace mozilla::net {

struct NoVarySearchData {
  enum class ParamsRule : uint8_t {
    ExactMatch,
    IgnoreAll,
    Blocklist,
    Allowlist
  };
  ParamsRule paramsRule = ParamsRule::ExactMatch;
  nsTArray<nsCString> paramNames;
  bool varyOnKeyOrder = true;
};

NoVarySearchData ParseNoVarySearchHeader(const nsACString& aHeader,
                                         bool* aParseError = nullptr);

nsLiteralCString NoVarySearchRuleLabel(NoVarySearchData::ParamsRule aRule);

bool URLsAreEquivalentModuloVariationConfig(nsIURI* aURIA, nsIURI* aURIB,
                                            const NoVarySearchData& aData);

nsresult ExtractNoVarySearchBasePath(nsIURI* aURI, nsACString& aBasePath);

}  
#endif
