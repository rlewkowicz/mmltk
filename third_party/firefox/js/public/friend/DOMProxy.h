/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_friend_DOMProxy_h
#define js_friend_DOMProxy_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint64_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Id.h"          // JS::PropertyKey
#include "js/RootingAPI.h"  // JS::Handle, JS::Heap
#include "js/Value.h"       // JS::UndefinedValue, JS::Value

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {


struct ExpandoAndGeneration {
  ExpandoAndGeneration() : expando(JS::UndefinedValue()), generation(0) {}

  void OwnerUnlinked() { ++generation; }

  static constexpr size_t offsetOfExpando() {
    return offsetof(ExpandoAndGeneration, expando);
  }

  static constexpr size_t offsetOfGeneration() {
    return offsetof(ExpandoAndGeneration, generation);
  }

  Heap<Value> expando;
  uint64_t generation;
};

enum class DOMProxyShadowsResult {
  ShadowCheckFailed,
  Shadows,
  DoesntShadow,
  DoesntShadowUnique,
  ShadowsViaDirectExpando,
  ShadowsViaIndirectExpando
};

using DOMProxyShadowsCheck = DOMProxyShadowsResult (*)(JSContext*,
                                                       Handle<JSObject*>,
                                                       Handle<JS::PropertyKey>);

extern JS_PUBLIC_API void SetDOMProxyInformation(
    const void* domProxyHandlerFamily,
    DOMProxyShadowsCheck domProxyShadowsCheck,
    const void* domRemoteProxyHandlerFamily);

}  

#endif  // js_friend_DOMProxy_h
