/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_AutoRestore_h_
#define mozilla_AutoRestore_h_

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS

namespace mozilla {

template <class T>
class MOZ_RAII AutoRestore {
 private:
  T& mLocation;
  T mValue;

 public:
  explicit AutoRestore(T& aValue) : mLocation(aValue), mValue(aValue) {}
  ~AutoRestore() { mLocation = mValue; }
  T SavedValue() const { return mValue; }
};

}  

#endif /* !defined(mozilla_AutoRestore_h_) */
