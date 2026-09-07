/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsNoDataProtocolContentPolicy_h_
#define nsNoDataProtocolContentPolicy_h_

#define NS_NODATAPROTOCOLCONTENTPOLICY_CID \
  {0xac9e3e82, 0xbfbd, 0x4f26, {0x94, 0x1e, 0xf5, 0x8c, 0x8e, 0xe1, 0x78, 0xc1}}
#define NS_NODATAPROTOCOLCONTENTPOLICY_CONTRACTID \
  "@mozilla.org/no-data-protocol-content-policy;1"

#include "nsIContentPolicy.h"

class nsNoDataProtocolContentPolicy final : public nsIContentPolicy {
  ~nsNoDataProtocolContentPolicy() = default;

 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSICONTENTPOLICY

  nsNoDataProtocolContentPolicy() = default;
};

#endif /* nsNoDataProtocolContentPolicy_h_ */
