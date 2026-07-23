/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Promise_inl_h
#define builtin_Promise_inl_h

#include "js/Promise.h"  // JS::PromiseState

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "js/RootingAPI.h"     // JS::Handle
#include "vm/JSContext.h"      // JSContext
#include "vm/PromiseObject.h"  // js::PromiseObject

namespace js {

inline void SetSettledPromiseIsHandled(
    JSContext* cx, JS::Handle<PromiseObject*> unwrappedPromise) {
  MOZ_ASSERT(unwrappedPromise->state() != JS::PromiseState::Pending);
  unwrappedPromise->setHandled();
  cx->runtime()->removeUnhandledRejectedPromise(cx, unwrappedPromise);
}

inline void SetAnyPromiseIsHandled(
    JSContext* cx, JS::Handle<PromiseObject*> unwrappedPromise) {
  if (unwrappedPromise->state() != JS::PromiseState::Pending) {
    cx->runtime()->removeUnhandledRejectedPromise(cx, unwrappedPromise);
  }
  unwrappedPromise->setHandled();
}

}  

#endif  // builtin_Promise_inl_h
