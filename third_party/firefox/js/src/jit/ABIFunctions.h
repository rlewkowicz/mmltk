/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ABIFunctions_h
#define jit_ABIFunctions_h

#include "jstypes.h"  // JS_FUNC_TO_DATA_PTR

struct JS_PUBLIC_API JSContext;

namespace JS {
class JS_PUBLIC_API Value;
}

namespace js {
namespace jit {

template <typename Sig, Sig fun>
struct ABIFunctionData {
  static const bool registered = false;
};

template <typename Sig, Sig fun>
struct ABIFunction {
  void* address() const { return JS_FUNC_TO_DATA_PTR(void*, fun); }

  static_assert(ABIFunctionData<Sig, fun>::registered,
                "ABI function is not registered.");
};

template <typename Sig>
struct ABIFunctionSignatureData {
  static const bool registered = false;
};

template <typename Sig>
struct ABIFunctionSignature {
  void* address(Sig fun) const { return JS_FUNC_TO_DATA_PTR(void*, fun); }

  static_assert(ABIFunctionSignatureData<Sig>::registered,
                "ABI function signature is not registered.");
};

struct DynFn {
  void* address;
};

#ifdef JS_SIMULATOR
bool CallAnyNative(JSContext* cx, unsigned argc, JS::Value* vp);
const void* RedirectedCallAnyNative();
#endif

}  
}  

#endif /* jit_VMFunctions_h */
