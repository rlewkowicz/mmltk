/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DDLogObject_h_
#define DDLogObject_h_

#include "nsString.h"

namespace mozilla {

class DDLogObject {
 public:
  DDLogObject() : mTypeName("<unset>"), mPointer(nullptr) {}

  DDLogObject(const char* aTypeName, const void* aPointer)
      : mTypeName(aTypeName), mPointer(aPointer) {
    MOZ_ASSERT(aTypeName);
    MOZ_ASSERT(aPointer);
  }

  void Set(const char* aTypeName, const void* aPointer) {
    MOZ_ASSERT(aTypeName);
    MOZ_ASSERT(aPointer);
    mTypeName = aTypeName;
    mPointer = aPointer;
  }

  const void* Pointer() const { return mPointer; }

  const char* TypeName() const {
    MOZ_ASSERT(mPointer);
    return mTypeName;
  }

  bool operator==(const DDLogObject& a) const {
    return mPointer == a.mPointer && (!mPointer || mTypeName == a.mTypeName);
  }

  void AppendPrintf(nsCString& mString) const;
  nsCString Printf() const;

 private:
  const char* mTypeName;
  const void* mPointer;
};

}  

#endif  // DDLogObject_h_
