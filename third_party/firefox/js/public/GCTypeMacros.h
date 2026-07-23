/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef GCTypeMacros_h
#define GCTypeMacros_h

#include "jstypes.h"  // JS_PUBLIC_API

class JS_PUBLIC_API JSAtom;
class JS_PUBLIC_API JSFunction;
class JS_PUBLIC_API JSObject;
class JS_PUBLIC_API JSScript;
class JS_PUBLIC_API JSString;

namespace JS {
class JS_PUBLIC_API BigInt;
class JS_PUBLIC_API PropertyKey;
class JS_PUBLIC_API Symbol;
class JS_PUBLIC_API Value;
}  

#define JS_FOR_EACH_PUBLIC_GC_POINTER_TYPE(D) \
  D(JS::BigInt*)                              \
  D(JS::Symbol*)                              \
  D(JSAtom*)                                  \
  D(JSFunction*)                              \
  D(JSLinearString*)                          \
  D(JSObject*)                                \
  D(JSScript*)                                \
  D(JSString*)

#define JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(D) \
  D(JS::Value)                                       \
  D(JS::PropertyKey)  // i.e. jsid

#endif  // GCTypeMacros_h
