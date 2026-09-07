/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_AnimationStorageData_h
#define mozilla_layers_AnimationStorageData_h

#include "mozilla/dom/Nullable.h"
#include "mozilla/ServoStyleConsts.h"       // for ComputedTimingFunction
#include "mozilla/layers/LayersMessages.h"  // for TransformData, etc
#include "mozilla/layers/LayersTypes.h"     // for LayersId
#include "mozilla/TimeStamp.h"              // for TimeStamp
#include "mozilla/TimingParams.h"

namespace mozilla {

namespace dom {
enum class CompositeOperation : uint8_t;
enum class IterationCompositeOperation : uint8_t;
};  

namespace layers {

struct PropertyAnimation {
  struct SegmentData {
    RefPtr<StyleAnimationValue> mStartValue;
    RefPtr<StyleAnimationValue> mEndValue;
    Maybe<mozilla::StyleComputedTimingFunction> mFunction;
    float mStartPortion;
    float mEndPortion;
    dom::CompositeOperation mStartComposite;
    dom::CompositeOperation mEndComposite;
  };
  nsTArray<SegmentData> mSegments;
  TimingParams mTiming;

  dom::Nullable<double> mProgressOnLastCompose;
  uint64_t mCurrentIterationOnLastCompose = 0;
  uint32_t mSegmentIndexOnLastCompose = 0;
  dom::Nullable<double> mPortionInSegmentOnLastCompose;

  TimeStamp mOriginTime;
  Maybe<TimeDuration> mStartTime;
  TimeDuration mHoldTime;
  float mPlaybackRate;
  dom::IterationCompositeOperation mIterationComposite;
  bool mIsNotPlaying;

  Maybe<ScrollTimelineOptions> mScrollTimelineOptions;

  void ResetLastCompositionValues() {
    mCurrentIterationOnLastCompose = 0;
    mSegmentIndexOnLastCompose = 0;
    mProgressOnLastCompose.SetNull();
    mPortionInSegmentOnLastCompose.SetNull();
  }
};

struct PropertyAnimationGroup {
  NonCustomCSSPropertyId mProperty;

  nsTArray<PropertyAnimation> mAnimations;
  RefPtr<StyleAnimationValue> mBaseStyle;

  bool IsEmpty() const { return mAnimations.IsEmpty(); }
  void Clear() {
    mAnimations.Clear();
    mBaseStyle = nullptr;
  }
  void ResetLastCompositionValues() {
    for (PropertyAnimation& animation : mAnimations) {
      animation.ResetLastCompositionValues();
    }
  }
};

struct AnimationStorageData {
  nsTArray<PropertyAnimationGroup> mAnimation;
  Maybe<TransformData> mTransformData;
  RefPtr<gfx::Path> mCachedMotionPath;
  LayersId mLayersId;

  AnimationStorageData() = default;
  AnimationStorageData(AnimationStorageData&& aOther) = default;
  AnimationStorageData& operator=(AnimationStorageData&& aOther) = default;

  AnimationStorageData(const AnimationStorageData& aOther) = delete;
  AnimationStorageData& operator=(const AnimationStorageData& aOther) = delete;

  bool IsEmpty() const { return mAnimation.IsEmpty(); }
  void Clear() {
    mAnimation.Clear();
    mTransformData.reset();
    mCachedMotionPath = nullptr;
  }
};

}  
}  

#endif  // mozilla_layers_AnimationStorageData_h
