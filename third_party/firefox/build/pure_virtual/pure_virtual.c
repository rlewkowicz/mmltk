/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <mozilla/Assertions.h>

#ifdef _MSC_VER
int __cdecl _purecall() { MOZ_CRASH("pure virtual call"); }
#else
__attribute__((visibility("hidden"))) void __cxa_pure_virtual() {
  MOZ_CRASH("pure virtual call");
}
#endif
