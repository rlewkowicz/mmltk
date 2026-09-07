/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_AnimationEventDispatcher_h
#define mozilla_AnimationEventDispatcher_h

#include "mozilla/AnimationComparator.h"
#include "mozilla/Attributes.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/Variant.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/KeyframeEffect.h"
#include "nsCycleCollectionParticipant.h"
#include "nsPresContext.h"

class nsRefreshDriver;

namespace mozilla {

struct AnimationEventInfo {
  struct CssAnimationOrTransitionData {
    OwningAnimationTarget mTarget;
    const EventMessage mMessage;
    const double mElapsedTime;
    const uint64_t mAnimationIndex;
    const TimeStamp mEventEnqueueTimeStamp{TimeStamp::Now()};
  };

  struct CssAnimationData : public CssAnimationOrTransitionData {
    const RefPtr<nsAtom> mAnimationName;
  };

  struct CssTransitionData : public CssAnimationOrTransitionData {
    const CSSPropertyId mProperty;
  };

  struct WebAnimationData {
    const RefPtr<nsAtom> mOnEvent;
    const dom::Nullable<double> mCurrentTime;
    const dom::Nullable<double> mTimelineTime;
    const TimeStamp mEventEnqueueTimeStamp{TimeStamp::Now()};
  };

  using Data = Variant<CssAnimationData, CssTransitionData, WebAnimationData>;

  RefPtr<dom::Animation> mAnimation;
  TimeStamp mScheduledEventTimeStamp;
  Data mData;

  OwningAnimationTarget* GetOwningAnimationTarget() {
    if (mData.is<CssAnimationData>()) {
      return &mData.as<CssAnimationData>().mTarget;
    }
    if (mData.is<CssTransitionData>()) {
      return &mData.as<CssTransitionData>().mTarget;
    }
    return nullptr;
  }

  Maybe<dom::Animation::EventContext> GetEventContext() const {
    if (mData.is<CssAnimationData>()) {
      const auto& data = mData.as<CssAnimationData>();
      return Some(dom::Animation::EventContext{
          NonOwningAnimationTarget(data.mTarget), data.mAnimationIndex});
    }
    if (mData.is<CssTransitionData>()) {
      const auto& data = mData.as<CssTransitionData>();
      return Some(dom::Animation::EventContext{
          NonOwningAnimationTarget(data.mTarget), data.mAnimationIndex});
    }
    return Nothing();
  }

  AnimationEventInfo(RefPtr<nsAtom> aAnimationName,
                     const NonOwningAnimationTarget& aTarget,
                     EventMessage aMessage, double aElapsedTime,
                     uint64_t aAnimationIndex,
                     const TimeStamp& aScheduledEventTimeStamp,
                     dom::Animation* aAnimation)
      : mAnimation(aAnimation),
        mScheduledEventTimeStamp(aScheduledEventTimeStamp),
        mData(CssAnimationData{
            {OwningAnimationTarget(aTarget.mElement, aTarget.mPseudoRequest),
             aMessage, aElapsedTime, aAnimationIndex},
            std::move(aAnimationName)}) {}

  AnimationEventInfo(const CSSPropertyId& aProperty,
                     const NonOwningAnimationTarget& aTarget,
                     EventMessage aMessage, double aElapsedTime,
                     uint64_t aTransitionGeneration,
                     const TimeStamp& aScheduledEventTimeStamp,
                     dom::Animation* aAnimation)
      : mAnimation(aAnimation),
        mScheduledEventTimeStamp(aScheduledEventTimeStamp),
        mData(CssTransitionData{
            {OwningAnimationTarget(aTarget.mElement, aTarget.mPseudoRequest),
             aMessage, aElapsedTime, aTransitionGeneration},
            aProperty}) {}

  AnimationEventInfo(nsAtom* aOnEvent,
                     const dom::Nullable<double>& aCurrentTime,
                     const dom::Nullable<double>& aTimelineTime,
                     TimeStamp&& aScheduledEventTimeStamp,
                     dom::Animation* aAnimation)
      : mAnimation(aAnimation),
        mScheduledEventTimeStamp(std::move(aScheduledEventTimeStamp)),
        mData(WebAnimationData{RefPtr{aOnEvent}, aCurrentTime, aTimelineTime}) {
  }

  AnimationEventInfo(const AnimationEventInfo& aOther) = delete;
  AnimationEventInfo& operator=(const AnimationEventInfo& aOther) = delete;

  AnimationEventInfo(AnimationEventInfo&& aOther) = default;
  AnimationEventInfo& operator=(AnimationEventInfo&& aOther) = default;

  int32_t Compare(const AnimationEventInfo& aOther,
                  nsContentUtils::NodeIndexCache& aCache) const {
    if (mScheduledEventTimeStamp != aOther.mScheduledEventTimeStamp) {
      if (mScheduledEventTimeStamp.IsNull()) {
        return -1;
      }
      if (aOther.mScheduledEventTimeStamp.IsNull()) {
        return 1;
      }
      return mScheduledEventTimeStamp < aOther.mScheduledEventTimeStamp ? -1
                                                                        : 1;
    }

    if (IsWebAnimationEvent() != aOther.IsWebAnimationEvent()) {
      return IsWebAnimationEvent() ? -1 : 1;
    }

    return mAnimation->CompareCompositeOrder(GetEventContext(),
                                             *aOther.mAnimation,
                                             aOther.GetEventContext(), aCache);
  }

  bool IsWebAnimationEvent() const { return mData.is<WebAnimationData>(); }

  MOZ_CAN_RUN_SCRIPT_BOUNDARY void Dispatch(nsPresContext* aPresContext);
};

class AnimationEventDispatcher final {
 public:
  explicit AnimationEventDispatcher(nsPresContext* aPresContext)
      : mPresContext(aPresContext), mIsSorted(true) {}

  NS_INLINE_DECL_CYCLE_COLLECTING_NATIVE_REFCOUNTING(AnimationEventDispatcher)
  NS_DECL_CYCLE_COLLECTION_NATIVE_CLASS(AnimationEventDispatcher)

  void Disconnect();

  void QueueEvent(AnimationEventInfo&& aEvent);
  void QueueEvents(nsTArray<AnimationEventInfo>&& aEvents);

  void DispatchEvents() {
    if (!mPresContext || mPendingEvents.IsEmpty()) {
      return;
    }

    SortEvents();

    EventArray events = std::move(mPendingEvents);
    for (AnimationEventInfo& info : events) {
      info.Dispatch(mPresContext);

      if (!mPresContext) {
        break;
      }
    }
  }

  void ClearEventQueue() {
    mPendingEvents.Clear();
    mIsSorted = true;
  }
  bool HasQueuedEvents() const { return !mPendingEvents.IsEmpty(); }

  bool HasQueuedEventsFor(const dom::Animation* aAnimation) const {
    for (const AnimationEventInfo& info : mPendingEvents) {
      if (info.mAnimation.get() == aAnimation) {
        return true;
      }
    }
    return false;
  }

 private:
  ~AnimationEventDispatcher() = default;

  void SortEvents() {
    if (mIsSorted) {
      return;
    }

    struct AnimationEventInfoComparator {
      mutable nsContentUtils::NodeIndexCache mCache;

      bool LessThan(const AnimationEventInfo& aOne,
                    const AnimationEventInfo& aOther) const {
        return aOne.Compare(aOther, mCache) < 0;
      }
    };

    mPendingEvents.StableSort(AnimationEventInfoComparator());
    mIsSorted = true;
  }
  void ScheduleDispatch();

  nsPresContext* mPresContext;
  using EventArray = nsTArray<AnimationEventInfo>;
  EventArray mPendingEvents;
  bool mIsSorted;
};

}  

#endif  // mozilla_AnimationEventDispatcher_h
