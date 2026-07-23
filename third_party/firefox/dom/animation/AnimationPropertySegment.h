/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_AnimationPropertySegment_h
#define mozilla_dom_AnimationPropertySegment_h

#include "mozilla/Maybe.h"
#include "mozilla/ServoStyleConsts.h"
#include "mozilla/StyleAnimationValue.h"           // For AnimationValue
#include "mozilla/dom/BaseKeyframeTypesBinding.h"  // For CompositeOperation

namespace mozilla {

struct AnimationPropertySegment {
  float mFromKey, mToKey;
  AnimationValue mFromValue, mToValue;

  Maybe<StyleComputedTimingFunction> mTimingFunction;
  dom::CompositeOperation mFromComposite = dom::CompositeOperation::Replace;
  dom::CompositeOperation mToComposite = dom::CompositeOperation::Replace;

  bool HasReplaceableValues() const {
    return HasReplaceableFromValue() && HasReplaceableToValue();
  }

  bool HasReplaceableFromValue() const {
    return !mFromValue.IsNull() &&
           mFromComposite == dom::CompositeOperation::Replace;
  }

  bool HasReplaceableToValue() const {
    return !mToValue.IsNull() &&
           mToComposite == dom::CompositeOperation::Replace;
  }

  bool operator==(const AnimationPropertySegment& aOther) const {
    return mFromKey == aOther.mFromKey && mToKey == aOther.mToKey &&
           mFromValue == aOther.mFromValue && mToValue == aOther.mToValue &&
           mTimingFunction == aOther.mTimingFunction &&
           mFromComposite == aOther.mFromComposite &&
           mToComposite == aOther.mToComposite;
  }
  bool operator!=(const AnimationPropertySegment& aOther) const {
    return !(*this == aOther);
  }
};

}  

#endif  // mozilla_dom_AnimationPropertySegment_h
