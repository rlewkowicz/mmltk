/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_Symbol_h
#define js_Symbol_h

#include "js/shadow/Symbol.h"  // JS::shadow::Symbol::WellKnownAPILimit

#include <stddef.h>  // size_t
#include <stdint.h>  // uintptr_t, uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

namespace JS {

class JS_PUBLIC_API Symbol;

extern JS_PUBLIC_API Symbol* NewSymbol(JSContext* cx,
                                       Handle<JSString*> description);

extern JS_PUBLIC_API Symbol* GetSymbolFor(JSContext* cx, Handle<JSString*> key);

extern JS_PUBLIC_API JSString* GetSymbolDescription(Handle<Symbol*> symbol);

#define JS_FOR_EACH_WELL_KNOWN_SYMBOL(MACRO)      \
  MACRO(isConcatSpreadable)                       \
  MACRO(iterator)                                 \
  MACRO(match)                                    \
  MACRO(replace)                                  \
  MACRO(search)                                   \
  MACRO(species)                                  \
  MACRO(hasInstance)                              \
  MACRO(split)                                    \
  MACRO(toPrimitive)                              \
  MACRO(toStringTag)                              \
  MACRO(unscopables)                              \
  MACRO(asyncIterator)                            \
  MACRO(matchAll)                                 \
  IF_EXPLICIT_RESOURCE_MANAGEMENT(MACRO(dispose)) \
  IF_EXPLICIT_RESOURCE_MANAGEMENT(MACRO(asyncDispose))

enum class SymbolCode : uint32_t {
#define JS_DEFINE_SYMBOL_ENUM(name) name,
  JS_FOR_EACH_WELL_KNOWN_SYMBOL(
      JS_DEFINE_SYMBOL_ENUM)  
#undef JS_DEFINE_SYMBOL_ENUM
  Limit,
  WellKnownAPILimit = JS::shadow::Symbol::WellKnownAPILimit,
  PrivateNameSymbol = 0xfffffffd,  
  InSymbolRegistry =
      0xfffffffe,            
  UniqueSymbol = 0xffffffff  
};

const size_t WellKnownSymbolLimit = size_t(SymbolCode::Limit);

extern JS_PUBLIC_API SymbolCode GetSymbolCode(Handle<Symbol*> symbol);

extern JS_PUBLIC_API Symbol* GetWellKnownSymbol(JSContext* cx,
                                                SymbolCode which);

inline bool PropertySpecNameIsSymbol(uintptr_t name) {
  return name != 0 && name - 1 < WellKnownSymbolLimit;
}

}  

#endif /* js_Symbol_h */
