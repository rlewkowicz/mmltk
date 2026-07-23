/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_ARTIFICIALFAILURE_H_
#define DOM_QUOTA_ARTIFICIALFAILURE_H_

#include <cstdint>

#include "nsIQuotaArtificialFailure.h"

enum class nsresult : uint32_t;

namespace mozilla {

struct Ok;
template <typename V, typename E>
class Result;

}  

namespace mozilla::dom::quota {

Result<Ok, nsresult> ArtificialFailure(
    nsIQuotaArtificialFailure::Category aCategory);

}  

#endif  // DOM_QUOTA_ARTIFICIALFAILURE_H_
