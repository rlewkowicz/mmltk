/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_KnownClass_h
#define jit_KnownClass_h

#include "jspubtd.h"

namespace js {
namespace jit {

class MDefinition;

enum class KnownClass {
  PlainObject,
  Array,
  Function,
  RegExp,
  ArrayIterator,
  StringIterator,
  RegExpStringIterator,
  None
};

KnownClass GetObjectKnownClass(const MDefinition* def);
const JSClass* GetObjectKnownJSClass(const MDefinition* def);

}  
}  

#endif  // jit_KnownClass_h
