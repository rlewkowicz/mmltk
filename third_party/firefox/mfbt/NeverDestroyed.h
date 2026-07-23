/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_NeverDestroyed_h
#define mozilla_NeverDestroyed_h

#include <new>
#include <type_traits>
#include <utility>
#include "mozilla/Attributes.h"

namespace mozilla {

template <typename T>
class MOZ_STATIC_LOCAL_CLASS MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS
NeverDestroyed {
 public:
  static_assert(
      !std::is_trivially_destructible_v<T>,
      "NeverDestroyed is unnecessary for trivially destructable types");

  template <typename... U>
  explicit NeverDestroyed(U&&... aArgs) {
    new (mStorage) T(std::forward<U>(aArgs)...);
  }

  const T& operator*() const { return *get(); }
  T& operator*() { return *get(); }

  const T* operator->() const { return get(); }
  T* operator->() { return get(); }

  const T* get() const { return reinterpret_cast<const T*>(mStorage); }
  T* get() { return reinterpret_cast<T*>(mStorage); }

  NeverDestroyed(const NeverDestroyed&) = delete;
  NeverDestroyed& operator=(const NeverDestroyed&) = delete;

 private:
  alignas(T) char mStorage[sizeof(T)];
};

};  

#endif  // mozilla_NeverDestroyed_h
