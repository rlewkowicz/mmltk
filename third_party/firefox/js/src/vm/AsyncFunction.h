/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_AsyncFunction_h
#define vm_AsyncFunction_h

#include "mozilla/Attributes.h"  // MOZ_RAII

#include "js/Class.h"
#include "vm/GeneratorObject.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PromiseObject.h"


namespace js {

class AsyncFunctionGeneratorObject;

extern const JSClass AsyncFunctionClass;

[[nodiscard]] bool AsyncFunctionAwaitedFulfilled(
    JSContext* cx, Handle<AsyncFunctionGeneratorObject*> generator,
    HandleValue value);

[[nodiscard]] bool AsyncFunctionAwaitedRejected(
    JSContext* cx, Handle<AsyncFunctionGeneratorObject*> generator,
    HandleValue reason);

JSObject* AsyncFunctionResolve(JSContext* cx,
                               Handle<AsyncFunctionGeneratorObject*> generator,
                               HandleValue value);

JSObject* AsyncFunctionReject(JSContext* cx,
                              Handle<AsyncFunctionGeneratorObject*> generator,
                              HandleValue reason, HandleValue stack);

class AsyncFunctionGeneratorObject : public AbstractGeneratorObject {
 public:
  enum {
    PROMISE_SLOT = AbstractGeneratorObject::RESERVED_SLOTS,

    RESERVED_SLOTS
  };

  static const JSClass class_;
  static const JSClassOps classOps_;

  static AsyncFunctionGeneratorObject* create(JSContext* cx,
                                              HandleFunction asyncGen);

  static AsyncFunctionGeneratorObject* create(JSContext* cx,
                                              Handle<ModuleObject*> module);

  PromiseObject* promise() {
    return &getFixedSlot(PROMISE_SLOT).toObject().as<PromiseObject>();
  }
};

class MOZ_RAII AutoAsyncResumeDepth {
  JSContext* cx_;

 public:
  explicit AutoAsyncResumeDepth(JSContext* cx) : cx_(cx) {
    cx_->asyncResumeDepth++;
  }
  ~AutoAsyncResumeDepth() { cx_->asyncResumeDepth--; }
};

}  

#endif /* vm_AsyncFunction_h */
