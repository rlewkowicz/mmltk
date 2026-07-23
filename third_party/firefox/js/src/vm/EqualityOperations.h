/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef vm_EqualityOperations_h
#define vm_EqualityOperations_h

#include "jstypes.h"        // JS_PUBLIC_API
#include "js/RootingAPI.h"  // JS::Handle
#include "js/Value.h"       // JS::Value

struct JS_PUBLIC_API JSContext;

namespace js {

extern bool ConstantStrictEqual(const JS::Value& val, uint16_t operand);

extern bool StrictlyEqual(JSContext* cx, const JS::Value& lval,
                          const JS::Value& rval, bool* equal);

extern bool LooselyEqual(JSContext* cx, JS::Handle<JS::Value> lval,
                         JS::Handle<JS::Value> rval, bool* equal);

extern bool SameValue(JSContext* cx, const JS::Value& v1, const JS::Value& v2,
                      bool* same);

extern bool SameValueZero(JSContext* cx, const JS::Value& v1,
                          const JS::Value& v2, bool* same);

inline bool CanUseBitwiseCompareForStrictlyEqual(const JS::Value& v) {
  return v.isObject() || v.isSymbol() || v.isNullOrUndefined() || v.isBoolean();
}

}  

#endif  // vm_EqualityOperations_h
