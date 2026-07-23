/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BuiltinObjectKind_h
#define vm_BuiltinObjectKind_h

#include <stdint.h>

#include "jstypes.h"

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace js {

namespace frontend {
class TaggedParserAtomIndex;
}

class GlobalObject;

enum class BuiltinObjectKind : uint8_t {
  Array,
  Map,
  Promise,
  RegExp,
  Set,
  Symbol,

  FunctionPrototype,
  IteratorPrototype,

  None,
};

BuiltinObjectKind BuiltinConstructorForName(
    frontend::TaggedParserAtomIndex name);

BuiltinObjectKind BuiltinPrototypeForName(frontend::TaggedParserAtomIndex name);

JSObject* MaybeGetBuiltinObject(GlobalObject* global, BuiltinObjectKind kind);

JSObject* GetOrCreateBuiltinObject(JSContext* cx, BuiltinObjectKind kind);

const char* BuiltinObjectName(BuiltinObjectKind kind);

}  

#endif /* vm_BuiltinObjectKind_h */
