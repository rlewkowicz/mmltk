/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef intl_components_ScopedICUObject_h
#define intl_components_ScopedICUObject_h


namespace mozilla::intl {

template <typename T, void(Delete)(T*)>
class ScopedICUObject {
  T* ptr_;

 public:
  explicit ScopedICUObject(T* ptr) : ptr_(ptr) {}

  ~ScopedICUObject() {
    if (ptr_) {
      Delete(ptr_);
    }
  }

  T* forget() {
    T* tmp = ptr_;
    ptr_ = nullptr;
    return tmp;
  }
};

}  

#endif /* intl_components_ScopedICUObject_h */
