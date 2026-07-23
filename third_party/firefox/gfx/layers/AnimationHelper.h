/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_AnimationHelper_h
#define mozilla_layers_AnimationHelper_h

#include "mozilla/dom/Nullable.h"
#include "mozilla/layers/AnimationStorageData.h"
#include "mozilla/layers/LayersMessages.h"     // for TransformData, etc
#include "mozilla/webrender/WebRenderTypes.h"  // for RenderRoot
#include "mozilla/TimeStamp.h"                 // for TimeStamp
#include "mozilla/TimingParams.h"

namespace mozilla::layers {
class Animation;
class APZSampler;
class CompositorAnimationStorage;
struct AnimatedValue;

using AnimationArray = nsTArray<layers::Animation>;
using SampledAnimationArray = AutoTArray<RefPtr<StyleAnimationValue>, 1>;

class AnimationHelper {
 public:
  struct SampleResult {
    enum class Type : uint8_t { None, Skipped, Sampled };
    enum class Reason : uint8_t { None, ScrollToDelayPhase };
    Type mType = Type::None;
    Reason mReason = Reason::None;

    SampleResult() = default;
    SampleResult(Type aType, Reason aReason) : mType(aType), mReason(aReason) {}

    static SampleResult Skipped() { return {Type::Skipped, Reason::None}; }
    static SampleResult Sampled() { return {Type::Sampled, Reason::None}; }

    bool IsNone() { return mType == Type::None; }
    bool IsSkipped() { return mType == Type::Skipped; }
    bool IsSampled() { return mType == Type::Sampled; }
  };

  static SampleResult SampleAnimationForEachNode(
      const APZSampler* aAPZSampler, const LayersId& aLayersId,
      const MutexAutoLock& aProofOfMapLock, TimeStamp aPreviousFrameTime,
      TimeStamp aCurrentFrameTime, const AnimatedValue* aPreviousValue,
      nsTArray<PropertyAnimationGroup>& aPropertyAnimationGroups,
      SampledAnimationArray& aAnimationValues );

  static AnimationStorageData ExtractAnimations(
      const LayersId& aLayersId, const AnimationArray& aAnimations,
      const CompositorAnimationStorage* aStorage,
      const TimeStamp& aPreviousSampleTime);

  static uint64_t GetNextCompositorAnimationsId();

  static gfx::Matrix4x4 ServoAnimationValueToMatrix4x4(
      const SampledAnimationArray& aValue, const TransformData& aTransformData,
      gfx::Path* aCachedMotionPath);

  static bool ShouldBeJank(const LayoutDeviceRect& aPrerenderedRect,
                           SideBits aOverflowedSides,
                           const gfx::Matrix4x4& aTransform,
                           const ParentLayerRect& aClipRect);
};

}  

#endif  // mozilla_layers_AnimationHelper_h
