/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_TrampolineNatives_h
#define jit_TrampolineNatives_h

#include <stdint.h>

#include "js/TypeDecls.h"


class JSJitInfo;

namespace JS {
class CallArgs;
}  

#define TRAMPOLINE_NATIVE_LIST(_) \
  _(ArraySort)                    \
  _(TypedArraySort)

namespace js {
namespace jit {

enum class TrampolineNative : uint16_t {
#define ADD_NATIVE(native) native,
  TRAMPOLINE_NATIVE_LIST(ADD_NATIVE)
#undef ADD_NATIVE
      Count
};

#define ADD_NATIVE(native) extern const JSJitInfo JitInfo_##native;
TRAMPOLINE_NATIVE_LIST(ADD_NATIVE)
#undef ADD_NATIVE

void SetTrampolineNativeJitEntry(JSContext* cx, JSFunction* fun,
                                 TrampolineNative native);

bool CallTrampolineNativeJitCode(JSContext* cx, TrampolineNative native,
                                 JS::CallArgs& args);

}  
}  

#endif /* jit_TrampolineNatives_h */
