/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_AnimationComparator_h
#define mozilla_AnimationComparator_h

#include "mozilla/dom/Animation.h"

namespace mozilla {


template <typename AnimationPtrType>
class AnimationPtrComparator {
  mutable nsContentUtils::NodeIndexCache mCache;

 public:
  bool Equals(const AnimationPtrType& a, const AnimationPtrType& b) const {
    return a == b;
  }

  bool LessThan(const AnimationPtrType& a, const AnimationPtrType& b) const {
    return a->CompareCompositeOrder(*b, mCache) < 0;
  }
};

}  

#endif  // mozilla_AnimationComparator_h
