/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PlainObject_h
#define vm_PlainObject_h

#include "ds/IdValuePair.h"
#include "gc/AllocKind.h"     // js::gc::AllocKind
#include "js/Class.h"         // JSClass
#include "js/RootingAPI.h"    // JS::Handle
#include "vm/JSObject.h"      // js::NewObjectKind
#include "vm/NativeObject.h"  // js::NativeObject

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSFunction;
class JS_PUBLIC_API JSObject;

namespace js {

struct IdValuePair;

class PlainObject : public NativeObject {
 public:
  static const JSClass class_;

  static inline js::PlainObject* createWithShape(JSContext* cx,
                                                 JS::Handle<SharedShape*> shape,
                                                 gc::AllocKind kind,
                                                 NewObjectKind newKind);

  static inline js::PlainObject* createWithShape(
      JSContext* cx, JS::Handle<SharedShape*> shape,
      NewObjectKind newKind = GenericObject);

  static inline PlainObject* createWithTemplate(
      JSContext* cx, JS::Handle<PlainObject*> templateObject);

  static js::PlainObject* createWithTemplateFromDifferentRealm(
      JSContext* cx, JS::Handle<PlainObject*> templateObject);

  inline gc::AllocKind allocKindForTenure() const;
};

extern bool CopyDataPropertiesNative(JSContext* cx,
                                     JS::Handle<PlainObject*> target,
                                     JS::Handle<NativeObject*> from,
                                     JS::Handle<PlainObject*> excludedItems,
                                     bool* optimized);

extern SharedShape* ThisShapeForFunction(JSContext* cx,
                                         JS::Handle<JSFunction*> callee,
                                         JS::Handle<JSObject*> newTarget);

extern PlainObject* NewPlainObject(JSContext* cx,
                                   const NewObjectOptions& options = {});

extern PlainObject* NewPlainObjectWithProto(
    JSContext* cx, HandleObject proto, const NewObjectOptions& options = {});

extern PlainObject* NewPlainObjectWithUniqueNames(
    JSContext* cx, Handle<IdValueVector> properties,
    const NewObjectOptions& options = {});

extern PlainObject* NewPlainObjectWithMaybeDuplicateKeys(
    JSContext* cx, Handle<IdValueVector> properties,
    const NewObjectOptions& options = {});

}  

#endif  // vm_PlainObject_h
