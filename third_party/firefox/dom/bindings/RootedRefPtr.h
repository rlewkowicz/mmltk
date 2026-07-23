/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_RootedRefPtr_h_
#define mozilla_RootedRefPtr_h_

#include "js/GCPolicyAPI.h"
#include "js/TypeDecls.h"
#include "mozilla/RefPtr.h"

namespace JS {
template <typename T>
struct GCPolicy<RefPtr<T>> {
  static RefPtr<T> initial() { return RefPtr<T>(); }

  static void trace(JSTracer* trc, RefPtr<T>* tp, const char* name) {
    if (*tp) {
      (*tp)->Trace(trc);
    }
  }

  static bool isValid(const RefPtr<T>& v) {
    return !v || GCPolicy<T>::isValid(*v.get());
  }
};
}  

namespace js {
template <typename T, typename Wrapper>
struct WrappedPtrOperations<RefPtr<T>, Wrapper> {
  operator T*() const { return static_cast<const Wrapper*>(this)->get(); }
};
}  

#endif /* mozilla_RootedRefPtr_h_ */
