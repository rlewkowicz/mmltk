/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_WeakMapPtr_h
#define js_WeakMapPtr_h

#include "jstypes.h"

#include "js/TypeDecls.h"

class JS_PUBLIC_API JSTracer;

namespace JS {

template <typename K, typename V>
class JS_PUBLIC_API WeakMapPtr {
 public:
  WeakMapPtr() : ptr(nullptr) {}
  bool init(JSContext* cx);
  bool initialized() { return ptr != nullptr; }
  void destroy();
  virtual ~WeakMapPtr() { MOZ_ASSERT(!initialized()); }
  void trace(JSTracer* trc);

  V lookup(const K& key);
  bool put(JSContext* cx, const K& key, const V& value);
  V removeValue(const K& key);

 private:
  void* ptr;

  WeakMapPtr(const WeakMapPtr& wmp) = delete;
  WeakMapPtr& operator=(const WeakMapPtr& wmp) = delete;
};

} 

#endif /* js_WeakMapPtr_h */
