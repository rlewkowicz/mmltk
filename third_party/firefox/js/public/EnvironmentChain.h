/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_EnvironmentChain_h
#define js_EnvironmentChain_h

#include "mozilla/Attributes.h"  // MOZ_RAII

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/GCVector.h"  // JS::RootedVector

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {

enum class SupportUnscopables : bool { No = false, Yes = true };

class MOZ_RAII JS_PUBLIC_API EnvironmentChain {
  JS::RootedObjectVector chain_;
  SupportUnscopables supportUnscopables_;

 public:
  EnvironmentChain(JSContext* cx, SupportUnscopables supportUnscopables)
      : chain_(cx), supportUnscopables_(supportUnscopables) {}

  EnvironmentChain(const EnvironmentChain&) = delete;
  void operator=(const EnvironmentChain&) = delete;

  [[nodiscard]] bool append(JSObject* obj) { return chain_.append(obj); }
  bool empty() const { return chain_.empty(); }
  size_t length() const { return chain_.length(); }

  RootedObjectVector& chain() { return chain_; }
  const RootedObjectVector& chain() const { return chain_; }

  void setSupportUnscopables(SupportUnscopables supportUnscopables) {
    supportUnscopables_ = supportUnscopables;
  }
  SupportUnscopables supportUnscopables() const { return supportUnscopables_; }
};

}  

#endif /* js_EnvironmentChain_h */
