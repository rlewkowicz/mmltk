/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PropertyKey_h
#define vm_PropertyKey_h

#include "mozilla/HashFunctions.h"  // mozilla::HashGeneric

#include "NamespaceImports.h"  // js::PropertyKey

#include "js/HashTable.h"   // js::DefaultHasher
#include "js/Id.h"          // JS::PropertyKey
#include "vm/StringType.h"  // JSAtom::hash
#include "vm/SymbolType.h"  // JS::Symbol::hash

namespace js {

static MOZ_ALWAYS_INLINE HashNumber HashPropertyKey(PropertyKey key) {
  if (MOZ_LIKELY(key.isAtom())) {
    return key.toAtom()->hash();
  }
  if (key.isSymbol()) {
    return key.toSymbol()->hash();
  }
  return mozilla::HashGeneric(key.asRawBits());
}

static MOZ_ALWAYS_INLINE HashNumber
HashAtomOrSymbolPropertyKey(PropertyKey key) {
  if (MOZ_LIKELY(key.isAtom())) {
    return key.toAtom()->hash();
  }
  return key.toSymbol()->hash();
}

static MOZ_ALWAYS_INLINE HashNumber HashPropertyKeyThreadSafe(PropertyKey key) {
  if (MOZ_LIKELY(key.isAtom())) {
    return key.toAtom()->asOffThreadAtom().hash();
  }
  if (key.isSymbol()) {
    return key.toSymbol()->hash();
  }
  return mozilla::HashGeneric(key.asRawBits());
}

}  

namespace mozilla {

template <>
struct DefaultHasher<JS::PropertyKey> {
  using Lookup = JS::PropertyKey;
  static HashNumber hash(JS::PropertyKey key) {
    return js::HashPropertyKey(key);
  }
  static bool match(JS::PropertyKey key1, JS::PropertyKey key2) {
    return key1 == key2;
  }
};

}  

#endif /* vm_PropertyKey_h */
