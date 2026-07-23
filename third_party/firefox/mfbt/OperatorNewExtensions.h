/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_OperatorNewExtensions_h
#define mozilla_OperatorNewExtensions_h

#include "mozilla/Assertions.h"

namespace mozilla {
enum NotNullTag {
  KnownNotNull,
};
}  

inline void* operator new(size_t, mozilla::NotNullTag, void* p) {
  MOZ_ASSERT(p);
  return p;
}

#endif  // mozilla_OperatorNewExtensions_h
