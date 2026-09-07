/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_WellKnownAtom_h
#define vm_WellKnownAtom_h

#include "mozilla/HashFunctions.h"  // mozilla::HashNumber, mozilla::HashStringKnownLength

#include <stdint.h>  // uint32_t

#include "js/ProtoKey.h"             // JS_FOR_EACH_PROTOTYPE
#include "js/Symbol.h"               // JS_FOR_EACH_WELL_KNOWN_SYMBOL
#include "vm/CommonPropertyNames.h"  // FOR_EACH_COMMON_PROPERTYNAME

namespace js {

enum class WellKnownAtomId : uint32_t {
#define ENUM_ENTRY_(NAME, _) NAME,
  FOR_EACH_COMMON_PROPERTYNAME(ENUM_ENTRY_)
#undef ENUM_ENTRY_

#define ENUM_ENTRY_(NAME, _) NAME,
      JS_FOR_EACH_PROTOTYPE(ENUM_ENTRY_)
#undef ENUM_ENTRY_

#define ENUM_ENTRY_(NAME) NAME,
          JS_FOR_EACH_WELL_KNOWN_SYMBOL(ENUM_ENTRY_)
#undef ENUM_ENTRY_

              Limit,
};

struct WellKnownAtomInfo {
  uint32_t length;
  mozilla::HashNumber hash;
  const char* content;
};

extern WellKnownAtomInfo wellKnownAtomInfos[];

inline const WellKnownAtomInfo& GetWellKnownAtomInfo(WellKnownAtomId atomId) {
  return wellKnownAtomInfos[uint32_t(atomId)];
}

} 

#endif  // vm_WellKnownAtom_h
