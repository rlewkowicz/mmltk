/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DDLogCategory_h_
#define DDLogCategory_h_

#include "mozilla/Assertions.h"
#include "mozilla/DefineEnum.h"

namespace mozilla {

MOZ_DEFINE_ENUM_CLASS(DDLogCategory,
                      (_Construction, _DerivedConstruction, _Destruction, _Link,
                       _Unlink, Property, Event, API, Log, MozLogError,
                       MozLogWarning, MozLogInfo, MozLogDebug, MozLogVerbose));

extern const char* const kDDLogCategoryShortStrings[kDDLogCategoryCount];

inline const char* ToShortString(DDLogCategory aCategory) {
  MOZ_ASSERT(static_cast<size_t>(aCategory) < kDDLogCategoryCount);
  return kDDLogCategoryShortStrings[static_cast<size_t>(aCategory)];
}

extern const char* const kDDLogCategoryLongStrings[kDDLogCategoryCount];

inline const char* ToLongString(DDLogCategory aCategory) {
  MOZ_ASSERT(static_cast<size_t>(aCategory) < kDDLogCategoryCount);
  return kDDLogCategoryLongStrings[static_cast<size_t>(aCategory)];
}

}  

#endif  // DDLogCategory_h_
