/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_JitCodeAPI_h
#define js_JitCodeAPI_h

#include "js/AllocPolicy.h"
#include "js/ColumnNumber.h"  // JS::LimitedColumnNumberOneOrigin
#include "js/Vector.h"

namespace JS {

struct JitCodeSourceInfo {
  uint32_t offset = 0;

  uint32_t lineno = 0;
  JS::LimitedColumnNumberOneOrigin colno;
};

using SourceInfoVector =
    js::Vector<JitCodeSourceInfo, 0, js::SystemAllocPolicy>;

struct JitCodeRecord {
  uint64_t code_addr = 0;
  uint32_t instructionSize = 0;

  SourceInfoVector sourceInfo;
};

JitCodeRecord* LookupJitCodeRecord(uint64_t addr);

}  

#endif /* js_JitCodeAPI_h */
