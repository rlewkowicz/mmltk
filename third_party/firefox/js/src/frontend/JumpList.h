/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_JumpList_h
#define frontend_JumpList_h

#include <stddef.h>  // ptrdiff_t

#include "frontend/BytecodeOffset.h"  // BytecodeOffset
#include "js/TypeDecls.h"             // jsbytecode

namespace js {
namespace frontend {


struct JumpTarget {
  BytecodeOffset offset = BytecodeOffset::invalidOffset();
};

struct JumpList {
  static const ptrdiff_t END_OF_LIST_DELTA = 0;

  JumpList() : offset(BytecodeOffset::invalidOffset()) {}

  BytecodeOffset offset;

  void push(jsbytecode* code, BytecodeOffset jumpOffset);

  void patchAll(jsbytecode* code, JumpTarget target);
};

} 
} 

#endif /* frontend_JumpList_h */
