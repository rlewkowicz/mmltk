/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_ServoUtils_h
#define mozilla_ServoUtils_h

#include "MainThreadUtils.h"
#include "mozilla/Assertions.h"

namespace mozilla {

void InitializeServo();
void ShutdownServo();
void AssertIsMainThreadOrServoFontMetricsLocked();

class ServoStyleSet;
extern ServoStyleSet* sInServoTraversal;
inline bool IsInServoTraversal() {
  MOZ_ASSERT(sInServoTraversal || NS_IsMainThread());
  return sInServoTraversal;
}
}  

#endif  // mozilla_ServoUtils_h
