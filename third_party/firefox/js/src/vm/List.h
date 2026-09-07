/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_List_h
#define vm_List_h

#include "NamespaceImports.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

namespace js {

class ListObject : public NativeObject {
 public:
  static const JSClass class_;

  [[nodiscard]] inline static ListObject* create(JSContext* cx);

  uint32_t length() const { return getDenseInitializedLength(); }

  bool isEmpty() const { return length() == 0; }

  const Value& get(uint32_t index) const { return getDenseElement(index); }

  template <class T>
  T& getAs(uint32_t index) const {
    return get(index).toObject().as<T>();
  }

  [[nodiscard]] inline bool append(JSContext* cx, Value value);

  [[nodiscard]] inline bool append(JSContext* cx, Value v1, Value v2);

  inline JS::Value popFirst(JSContext* cx);

  template <class T>
  inline T& popFirstAs(JSContext* cx);
};

}  

#endif  // vm_List_h
