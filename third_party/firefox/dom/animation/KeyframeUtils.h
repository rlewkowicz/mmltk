/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_KeyframeUtils_h
#define mozilla_KeyframeUtils_h

#include "NonCustomCSSPropertyId.h"
#include "js/RootingAPI.h"                 // For JS::Handle
#include "mozilla/Keyframe.h"              // For KeyframesOffsetHasAny
#include "mozilla/KeyframeEffectParams.h"  // For CompositeOperation
#include "nsTArrayForwardDeclare.h"        // For nsTArray

struct JSContext;
class JSObject;

namespace mozilla {
struct AnimationProperty;
class ComputedStyle;
struct CSSPropertyId;

class ErrorResult;
struct PropertyStyleAnimationValuePair;
struct PseudoStyleRequest;

enum class PseudoStyleType : uint8_t;
enum class StyleTimelineRangeName : uint8_t;

namespace dom {
struct AnimationRange;
class AnimationTimeline;
class Document;
class Element;
}  
}  

namespace mozilla {

using ComputedKeyframeValues = nsTArray<PropertyStyleAnimationValuePair>;

class KeyframeUtils {
 public:
  static nsTArray<Keyframe> GetKeyframesFromObject(
      JSContext* aCx, dom::Document* aDocument, JS::Handle<JSObject*> aFrames,
      const char* aContext, ErrorResult& aRv);

  static KeyframesOffsetHasAny ComputeMissingKeyframeOffsets(
      nsTArray<Keyframe>& aKeframes, const dom::AnimationTimeline* aTimeline,
      const dom::AnimationRange* aRange);

  static double GetComputedOffset(const Keyframe::OffsetType& aOffset,
                                  const dom::AnimationTimeline* aTimeline,
                                  const dom::AnimationRange* aRange);

  static nsTArray<AnimationProperty> GetAnimationPropertiesFromKeyframes(
      const nsTArray<Keyframe>& aKeyframes, dom::Element* aElement,
      const PseudoStyleRequest& aPseudoRequest, const ComputedStyle* aStyle,
      dom::CompositeOperation aEffectComposite,
      const dom::AnimationTimeline* aTimeline,
      const KeyframesOffsetHasAny& aOffsetHasAny);

  static bool IsAnimatableProperty(const CSSPropertyId& aProperty);

  struct GeneratedKeyframesStatus {
    bool mSkipGeneratedInitial = false;
    bool mSkipGeneratedFinal = false;
    bool ShouldSkip(const Keyframe& aKeyframe) const {
      return aKeyframe.mIsGenerated &&
             ((aKeyframe.mComputedOffset == 0.0 && mSkipGeneratedInitial) ||
              (aKeyframe.mComputedOffset == 1.0 && mSkipGeneratedFinal));
    }
  };
  static GeneratedKeyframesStatus CheckSkippableGeneratedKeyframes(
      const nsTArray<Keyframe>& aKeyframes,
      const dom::AnimationTimeline* aTimeline,
      const KeyframesOffsetHasAny& aOffsetHasAny);
};

}  

#endif  // mozilla_KeyframeUtils_h
