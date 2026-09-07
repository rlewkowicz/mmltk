/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef builtin_Boolean_h
#define builtin_Boolean_h

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;

namespace js {
class PropertyName;

extern PropertyName* BooleanToString(JSContext* cx, bool b);

}  

#endif /* builtin_Boolean_h */
