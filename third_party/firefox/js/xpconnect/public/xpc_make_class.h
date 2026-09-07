/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef xpc_make_class_h
#define xpc_make_class_h


#include "xpcpublic.h"
#include "mozilla/dom/DOMJSClass.h"

bool XPC_WN_MaybeResolvingPropertyStub(JSContext* cx, JS::HandleObject obj,
                                       JS::HandleId id, JS::HandleValue v);
bool XPC_WN_CannotModifyPropertyStub(JSContext* cx, JS::HandleObject obj,
                                     JS::HandleId id, JS::HandleValue v);

bool XPC_WN_MaybeResolvingDeletePropertyStub(JSContext* cx,
                                             JS::HandleObject obj,
                                             JS::HandleId id,
                                             JS::ObjectOpResult& result);
bool XPC_WN_CannotDeletePropertyStub(JSContext* cx, JS::HandleObject obj,
                                     JS::HandleId id,
                                     JS::ObjectOpResult& result);

bool XPC_WN_Shared_Enumerate(JSContext* cx, JS::HandleObject obj);

bool XPC_WN_NewEnumerate(JSContext* cx, JS::HandleObject obj,
                         JS::MutableHandleIdVector properties,
                         bool enumerableOnly);

bool XPC_WN_Helper_Resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                           bool* resolvedp);

void XPC_WN_Helper_Finalize(JS::GCContext* gcx, JSObject* obj);
void XPC_WN_NoHelper_Finalize(JS::GCContext* gcx, JSObject* obj);

bool XPC_WN_Helper_Call(JSContext* cx, unsigned argc, JS::Value* vp);

bool XPC_WN_Helper_Construct(JSContext* cx, unsigned argc, JS::Value* vp);

void XPCWrappedNative_Trace(JSTracer* trc, JSObject* obj);

extern const js::ClassExtension XPC_WN_JSClassExtension;

#define XPC_MAKE_CLASS_OPS(_flags)                                            \
  {                                                                           \
                                                             \
      ((_flags) & XPC_SCRIPTABLE_USE_JSSTUB_FOR_ADDPROPERTY) ? nullptr        \
      : ((_flags) & XPC_SCRIPTABLE_ALLOW_PROP_MODS_DURING_RESOLVE)            \
          ? XPC_WN_MaybeResolvingPropertyStub                                 \
          : XPC_WN_CannotModifyPropertyStub,                                  \
                                                                              \
                                                             \
      ((_flags) & XPC_SCRIPTABLE_USE_JSSTUB_FOR_DELPROPERTY) ? nullptr        \
      : ((_flags) & XPC_SCRIPTABLE_ALLOW_PROP_MODS_DURING_RESOLVE)            \
          ? XPC_WN_MaybeResolvingDeletePropertyStub                           \
          : XPC_WN_CannotDeletePropertyStub,                                  \
                                                                              \
                                                               \
      ((_flags) & XPC_SCRIPTABLE_WANT_NEWENUMERATE)                           \
          ? nullptr      \
          : XPC_WN_Shared_Enumerate,                                          \
                                                                              \
                                                            \
      ((_flags) & XPC_SCRIPTABLE_WANT_NEWENUMERATE) ? XPC_WN_NewEnumerate     \
                                                    : nullptr,                \
                                                                              \
                                                              \
      XPC_WN_Helper_Resolve,                                                  \
                                                                              \
                                                              \
      nullptr,                                                                \
                                                                              \
                                                                \
      ((_flags) & XPC_SCRIPTABLE_WANT_FINALIZE) ? XPC_WN_Helper_Finalize      \
                                                : XPC_WN_NoHelper_Finalize,   \
                                                                              \
                                                                    \
      ((_flags) & XPC_SCRIPTABLE_WANT_CALL) ? XPC_WN_Helper_Call : nullptr,   \
                                                                              \
                                                               \
      ((_flags) & XPC_SCRIPTABLE_WANT_CONSTRUCT) ? XPC_WN_Helper_Construct    \
                                                 : nullptr,                   \
                                                                              \
                                                                   \
      ((_flags) & XPC_SCRIPTABLE_IS_GLOBAL_OBJECT) ? JS_GlobalObjectTraceHook \
                                                   : XPCWrappedNative_Trace,  \
  }

#define XPC_MAKE_CLASS(_name, _flags, _classOps)                 \
  {                                                              \
                                                       \
      _name,                                                     \
                                                                 \
                                                      \
      JSCLASS_SLOT0_IS_NSISUPPORTS | JSCLASS_IS_WRAPPED_NATIVE | \
          JSCLASS_FOREGROUND_FINALIZE |                          \
          (((_flags) & XPC_SCRIPTABLE_IS_GLOBAL_OBJECT)          \
               ? XPCONNECT_GLOBAL_FLAGS                          \
               : JSCLASS_HAS_RESERVED_SLOTS(1)),                 \
                                                                 \
                                                       \
      _classOps,                                                 \
                                                                 \
                                                       \
      nullptr,                                                   \
                                                                 \
                                                        \
      &XPC_WN_JSClassExtension,                                  \
                                                                 \
                                                       \
      nullptr,                                                   \
  }

#endif
