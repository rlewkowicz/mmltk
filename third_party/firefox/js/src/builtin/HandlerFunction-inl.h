/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef builtin_HandlerFunction_inl_h
#define builtin_HandlerFunction_inl_h

#include <stddef.h>  // size_t

#include "gc/AllocKind.h"   // js::gc::AllocKind
#include "js/CallArgs.h"    // JS::CallArgs
#include "js/RootingAPI.h"  // JS::Handle
#include "js/Value.h"       // JS::ObjectValue
#include "vm/JSContext.h"   // JSContext
#include "vm/JSFunction.h"  // JSFunction, js::Native, js::NewNativeFunction
#include "vm/JSObject.h"    // JSObject, js::GenericObject
#include "vm/StringType.h"  // js::PropertyName

#include "vm/JSContext-inl.h"  // JSContext::check

namespace js {

constexpr size_t HandlerFunctionSlot_Target = 0;
constexpr size_t HandlerFunctionSlot_Extra = 1;

static_assert(HandlerFunctionSlot_Extra < FunctionExtended::NUM_EXTENDED_SLOTS,
              "handler function slots shouldn't exceed available extended "
              "slots");

[[nodiscard]] inline JSFunction* NewHandler(JSContext* cx, Native handler,
                                            JS::Handle<JSObject*> target) {
  cx->check(target);

  JS::Handle<PropertyName*> funName = cx->names().empty_;
  JSFunction* handlerFun = NewNativeFunction(
      cx, handler, 0, funName, gc::AllocKind::FUNCTION_EXTENDED, GenericObject);
  if (!handlerFun) {
    return nullptr;
  }
  handlerFun->setExtendedSlot(HandlerFunctionSlot_Target,
                              JS::ObjectValue(*target));
  return handlerFun;
}

[[nodiscard]] inline JSFunction* NewHandlerWithExtra(
    JSContext* cx, Native handler, JS::Handle<JSObject*> target,
    JS::Handle<JSObject*> extra) {
  cx->check(extra);
  JSFunction* handlerFun = NewHandler(cx, handler, target);
  if (handlerFun) {
    handlerFun->setExtendedSlot(HandlerFunctionSlot_Extra,
                                JS::ObjectValue(*extra));
  }
  return handlerFun;
}

[[nodiscard]] inline JSFunction* NewHandlerWithExtraValue(
    JSContext* cx, Native handler, JS::Handle<JSObject*> target,
    JS::Handle<JS::Value> extra) {
  cx->check(extra);
  JSFunction* handlerFun = NewHandler(cx, handler, target);
  if (handlerFun) {
    handlerFun->setExtendedSlot(HandlerFunctionSlot_Extra, extra);
  }
  return handlerFun;
}

template <class T>
[[nodiscard]] inline T* TargetFromHandler(const JS::CallArgs& args) {
  JSFunction& func = args.callee().as<JSFunction>();
  return &func.getExtendedSlot(HandlerFunctionSlot_Target).toObject().as<T>();
}

[[nodiscard]] inline JS::Value ExtraValueFromHandler(const JS::CallArgs& args) {
  JSFunction& func = args.callee().as<JSFunction>();
  return func.getExtendedSlot(HandlerFunctionSlot_Extra);
}

template <class T>
[[nodiscard]] inline T* ExtraFromHandler(const JS::CallArgs& args) {
  return &ExtraValueFromHandler(args).toObject().as<T>();
}

}  

#endif  // builtin_HandlerFunction_inl_h
