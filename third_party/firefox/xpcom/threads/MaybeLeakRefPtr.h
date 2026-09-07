/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef MaybeLeakRefPtr_h
#define MaybeLeakRefPtr_h

#include "mozilla/RefPtr.h"

namespace mozilla {

template <class T>
class MaybeLeakRefPtr : public RefPtr<T> {
 public:
  explicit MaybeLeakRefPtr(already_AddRefed<T>&& aPtr, bool aAutoRelease)
      : RefPtr<T>(std::move(aPtr)), mAutoRelease(aAutoRelease) {}
  MaybeLeakRefPtr(const MaybeLeakRefPtr&) = delete;
  MaybeLeakRefPtr& operator=(const MaybeLeakRefPtr&) = delete;

  ~MaybeLeakRefPtr() {
    if (!mAutoRelease) {
      RefPtr<T>::forget().leak();
    }
  }

 private:
  bool mAutoRelease = false;
};

}  

#endif  // MaybeLeakRefPtr_h
