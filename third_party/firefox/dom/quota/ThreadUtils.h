/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_THREADUTILS_H_
#define DOM_QUOTA_THREADUTILS_H_

#include <cstdint>
#include <functional>

#include "mozilla/StaticPrefsBase.h"

enum class nsresult : uint32_t;

namespace mozilla::dom::quota {

nsresult RunAfterProcessingCurrentEvent(std::function<void()>&& aCallback);

void SleepIfEnabled(StripAtomic<RelaxedAtomicUint32> aMirroredPrefValue);

}  

#endif  // DOM_QUOTA_THREADUTILS_H_
