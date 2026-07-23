/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Jit_h
#define jit_Jit_h

#include "jstypes.h"

struct JS_PUBLIC_API JSContext;

namespace js {

class RunState;

namespace jit {

enum class EnterJitStatus {
  Error,

  Ok,

  NotEntered,
};

extern bool EnterInterpreterEntryTrampoline(uint8_t* code, JSContext* cx,
                                            RunState* state);
extern EnterJitStatus MaybeEnterJit(JSContext* cx, RunState& state);

}  
}  

#endif /* jit_Jit_h */
