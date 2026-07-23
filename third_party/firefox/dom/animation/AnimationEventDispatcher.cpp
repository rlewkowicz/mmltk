/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/AnimationEventDispatcher.h"

#include "mozilla/Assertions.h"
#include "mozilla/ContentEvents.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/EventListenerManager.h"
#include "mozilla/StaticPrefs_layout.h"
#include "mozilla/dom/Animation.h"
#include "mozilla/dom/AnimationEffect.h"
#include "mozilla/dom/AnimationPlaybackEvent.h"
#include "mozilla/dom/CSSAnimation.h"
#include "mozilla/dom/CSSNumericValueBinding.h"
#include "mozilla/dom/CSSTransition.h"
#include "mozilla/dom/CSSUnitValue.h"
#include "mozilla/dom/ScrollTimeline.h"  // For PROGRESS_TIMELINE_DURATION_MILLISEC
#include "nsCSSProps.h"
#include "nsGlobalWindowInner.h"
#include "nsPresContext.h"
#include "nsRefreshDriver.h"

using namespace mozilla;

namespace mozilla {

NS_IMPL_CYCLE_COLLECTION_CLASS(AnimationEventDispatcher)
NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(AnimationEventDispatcher)
  tmp->ClearEventQueue();
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(AnimationEventDispatcher)
  for (auto& info : tmp->mPendingEvents) {
    if (OwningAnimationTarget* target = info.GetOwningAnimationTarget()) {
      ImplCycleCollectionTraverse(
          cb, target->mElement,
          "mozilla::AnimationEventDispatcher.mPendingEvents.mTarget");
    }
    ImplCycleCollectionTraverse(
        cb, info.mAnimation,
        "mozilla::AnimationEventDispatcher.mPendingEvents.mAnimation");
  }
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

void AnimationEventDispatcher::Disconnect() {
  ClearEventQueue();
  mPresContext = nullptr;
}

void AnimationEventDispatcher::QueueEvent(AnimationEventInfo&& aEvent) {
  const bool wasEmpty = mPendingEvents.IsEmpty();
  mPendingEvents.AppendElement(std::move(aEvent));
  mIsSorted = !wasEmpty;
  if (wasEmpty) {
    ScheduleDispatch();
  }
}

void AnimationEventDispatcher::QueueEvents(
    nsTArray<AnimationEventInfo>&& aEvents) {
  if (aEvents.IsEmpty()) {
    return;
  }
  const bool wasEmpty = mPendingEvents.IsEmpty();
  mPendingEvents.AppendElements(std::move(aEvents));
  mIsSorted = false;
  if (wasEmpty) {
    ScheduleDispatch();
  }
}

void AnimationEventDispatcher::ScheduleDispatch() {
  MOZ_ASSERT(mPresContext, "The pres context should be valid");
  mPresContext->RefreshDriver()->ScheduleRenderingPhase(
      RenderingPhase::UpdateAnimationsAndSendEvents);
}

void AnimationEventInfo::Dispatch(nsPresContext* aPresContext) {
  if (mData.is<WebAnimationData>()) {
    const auto& data = mData.as<WebAnimationData>();
    EventListenerManager* elm = mAnimation->GetExistingListenerManager();
    if (!elm || !elm->HasListenersFor(data.mOnEvent)) {
      return;
    }

    dom::AnimationPlaybackEventInit init;
    if (!data.mCurrentTime.IsNull()) {
      if (mAnimation->AcceptsPercentageBasedTime()) {
        const double progress =
            data.mCurrentTime.Value() /
            static_cast<double>(PROGRESS_TIMELINE_DURATION_MILLISEC) * 100.0;
        init.mCurrentTime.SetValue().SetAsCSSNumericValue() =
            dom::MakeCSSUnitValue(mAnimation->GetParentObject(),
                                  StyleNumericType::Percent(), progress,
                                  "percent"_ns);
      } else {
        init.mCurrentTime.SetValue().SetAsDouble() = data.mCurrentTime.Value();
      }
    }
    init.mTimelineTime = data.mTimelineTime;
    MOZ_ASSERT(nsDependentAtomString(data.mOnEvent).Find(u"on"_ns) == 0,
               "mOnEvent atom should start with 'on'!");
    RefPtr<dom::AnimationPlaybackEvent> event =
        dom::AnimationPlaybackEvent::Constructor(
            mAnimation, Substring(nsDependentAtomString(data.mOnEvent), 2),
            init);
    event->SetTrusted(true);
    event->WidgetEventPtr()->AssignEventTime(
        WidgetEventTime(data.mEventEnqueueTimeStamp));
    RefPtr target = mAnimation;
    EventDispatcher::DispatchDOMEvent(target, nullptr , event,
                                      aPresContext,
                                      nullptr );
    return;
  }

  if (mData.is<CssTransitionData>()) {
    const auto& data = mData.as<CssTransitionData>();
    nsPIDOMWindowInner* win =
        data.mTarget.mElement->OwnerDoc()->GetInnerWindow();
    if (win && !win->HasTransitionEventListeners()) {
      MOZ_ASSERT(data.mMessage == eTransitionStart ||
                 data.mMessage == eTransitionRun ||
                 data.mMessage == eTransitionEnd ||
                 data.mMessage == eTransitionCancel);
      return;
    }

    InternalTransitionEvent event(true, data.mMessage);
    data.mProperty.ToString(event.mPropertyName);
    event.mElapsedTime = data.mElapsedTime;
    event.mAnimation = mAnimation->AsCSSTransition();
    data.mTarget.mPseudoRequest.ToString(event.mPseudoElement);
    event.AssignEventTime(WidgetEventTime(data.mEventEnqueueTimeStamp));
    RefPtr target = data.mTarget.mElement;
    EventDispatcher::Dispatch(target, aPresContext, &event);
    return;
  }

  const auto& data = mData.as<CssAnimationData>();
  InternalAnimationEvent event(true, data.mMessage);
  data.mAnimationName->ToString(event.mAnimationName);
  event.mElapsedTime = data.mElapsedTime;
  event.mAnimation = mAnimation->AsCSSAnimation();
  data.mTarget.mPseudoRequest.ToString(event.mPseudoElement);
  event.AssignEventTime(WidgetEventTime(data.mEventEnqueueTimeStamp));
  RefPtr target = data.mTarget.mElement;
  EventDispatcher::Dispatch(target, aPresContext, &event);
}

}  
