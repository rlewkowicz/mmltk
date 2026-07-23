/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_RustCell_h
#define mozilla_RustCell_h

namespace mozilla {

template <typename T>
class RustCell {
 public:
  RustCell() : mValue() {}
  explicit RustCell(T aValue) : mValue(aValue) {}

  T Get() const { return mValue; }
  void Set(T aValue) { mValue = aValue; }

  T* AsPtr() { return &mValue; }
  const T* AsPtr() const { return &mValue; }

 private:
  T mValue;
};

}  

#endif  // mozilla_RustCell_h
