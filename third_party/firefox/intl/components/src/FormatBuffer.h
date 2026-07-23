/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef intl_components_FormatBuffer_h
#define intl_components_FormatBuffer_h


#include "nsTString.h"

namespace mozilla::intl {

template <typename T>
class nsTStringToBufferAdapter {
 public:
  using CharType = T;

  nsTStringToBufferAdapter(const nsTStringToBufferAdapter&) = delete;
  nsTStringToBufferAdapter& operator=(const nsTStringToBufferAdapter&) = delete;

  explicit nsTStringToBufferAdapter(nsTSubstring<CharType>& aString)
      : mString(aString) {}

  [[nodiscard]] bool reserve(size_t size) {
    return mString.SetLength(size, fallible);
  }

  CharType* data() { return mString.BeginWriting(); }

  size_t length() const { return mString.Length(); }

  size_t capacity() const {
    return mString.Length();
  }

  void written(size_t amount) {
    MOZ_ASSERT(amount <= mString.Length());
    mString.SetLength(amount);
  }

 private:
  nsTSubstring<CharType>& mString;
};

}  

#endif /* intl_components_FormatBuffer_h */
