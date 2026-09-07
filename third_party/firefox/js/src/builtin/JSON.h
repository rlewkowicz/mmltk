/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_JSON_h
#define builtin_JSON_h

#include "mozilla/Range.h"

#include "NamespaceImports.h"

#include "js/RootingAPI.h"

namespace js {

class StringBuilder;

extern const JSClass JSONClass;

enum class StringifyBehavior {
  Normal,

  RestrictedSafe,

  FastOnly,

  SlowOnly,

  Compare
};

extern bool Stringify(JSContext* cx, js::MutableHandleValue vp,
                      JSObject* replacer, const Value& space, StringBuilder& sb,
                      StringifyBehavior stringifyBehavior);

template <typename CharT>
extern bool ParseJSONWithReviver(JSContext* cx,
                                 const mozilla::Range<const CharT> chars,
                                 HandleValue reviver, MutableHandleValue vp);

}  

#endif /* builtin_JSON_h */
