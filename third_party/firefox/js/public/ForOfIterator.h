/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_ForOfIterator_h
#define js_ForOfIterator_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS

#include <stdint.h>  // UINT32_MAX, uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RootingAPI.h"  // JS::{Handle,Rooted}
#include "js/Value.h"       // JS::Value, JS::{,Mutable}Handle<JS::Value>

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {

class MOZ_STACK_CLASS JS_PUBLIC_API ForOfIterator {
 protected:
  JSContext* cx_;

  Rooted<JSObject*> iteratorOrArray_;
  Rooted<Value> nextMethod_;

  uint32_t arrayIndex_ = 0;
  bool isOptimizedArray_ = false;

  ForOfIterator(const ForOfIterator&) = delete;
  ForOfIterator& operator=(const ForOfIterator&) = delete;

 public:
  explicit ForOfIterator(JSContext* cx)
      : cx_(cx), iteratorOrArray_(cx), nextMethod_(cx) {}

  enum NonIterableBehavior { ThrowOnNonIterable, AllowNonIterable };

  [[nodiscard]] bool init(
      Handle<Value> iterable,
      NonIterableBehavior nonIterableBehavior = ThrowOnNonIterable);

  [[nodiscard]] bool next(MutableHandle<Value> val, bool* done);

  void closeThrow();

  bool valueIsIterable() const { return iteratorOrArray_ != nullptr; }

 private:
  inline bool nextFromOptimizedArray(MutableHandle<Value> val, bool* done);
};

}  

#endif  // js_ForOfIterator_h
