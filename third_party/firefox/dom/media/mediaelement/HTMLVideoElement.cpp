/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/HTMLVideoElement.h"

#include "mozilla/AppShutdown.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/HTMLVideoElementBinding.h"
#include <algorithm>
#include <limits>

#include "FrameStatistics.h"
#include "ImageContainer.h"
#include "MediaDecoder.h"
#include "MediaDecoderStateMachine.h"
#include "MediaError.h"
#include "VideoFrameContainer.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/dom/Performance.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/TimeRanges.h"
#include "mozilla/dom/VideoPlaybackQuality.h"
#include "mozilla/dom/VideoStreamTrack.h"
#include "mozilla/dom/WakeLock.h"
#include "mozilla/dom/power/PowerManagerService.h"
#include "nsError.h"
#include "nsGenericHTMLElement.h"
#include "nsGkAtoms.h"
#include "nsIHttpChannel.h"
#include "nsNodeInfoManager.h"
#include "nsRFPService.h"
#include "nsSize.h"
#include "nsThreadUtils.h"
#include "plbase64.h"
#include "prlock.h"

extern mozilla::LazyLogModule gMediaElementLog;
#define LOG(msg, ...)                                                         \
  MOZ_LOG_FMT(gMediaElementLog, LogLevel::Debug, "HTMLVideoElement={}, " msg, \
              fmt::ptr(this), ##__VA_ARGS__)

nsGenericHTMLElement* NS_NewHTMLVideoElement(
    already_AddRefed<mozilla::dom::NodeInfo> aNodeInfo,
    mozilla::dom::FromParser aFromParser) {
  RefPtr<mozilla::dom::NodeInfo> nodeInfo(aNodeInfo);
  auto* nim = nodeInfo->NodeInfoManager();
  mozilla::dom::HTMLVideoElement* element =
      new (nim) mozilla::dom::HTMLVideoElement(nodeInfo.forget());
  element->Init();
  return element;
}

namespace mozilla::dom {

nsresult HTMLVideoElement::Clone(mozilla::dom::NodeInfo* aNodeInfo,
                                 nsINode** aResult) const {
  *aResult = nullptr;
  RefPtr<mozilla::dom::NodeInfo> ni(aNodeInfo);
  auto* nim = ni->NodeInfoManager();
  HTMLVideoElement* it = new (nim) HTMLVideoElement(ni.forget());
  it->Init();
  nsCOMPtr<nsINode> kungFuDeathGrip = it;
  nsresult rv = const_cast<HTMLVideoElement*>(this)->CopyInnerTo(it);
  if (NS_SUCCEEDED(rv)) {
    kungFuDeathGrip.swap(*aResult);
  }
  return rv;
}

nsresult HTMLVideoElement::CopyInnerTo(Element* aDest) {
  nsresult rv = HTMLMediaElement::CopyInnerTo(aDest);
  NS_ENSURE_SUCCESS(rv, rv);
  return rv;
}

NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED_0(HTMLVideoElement,
                                               HTMLMediaElement)

NS_IMPL_CYCLE_COLLECTION_CLASS(HTMLVideoElement)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(HTMLVideoElement)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mVideoFrameRequestManager)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END_INHERITED(HTMLMediaElement)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(HTMLVideoElement,
                                                  HTMLMediaElement)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mVideoFrameRequestManager)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

HTMLVideoElement::HTMLVideoElement(already_AddRefed<NodeInfo> aNodeInfo)
    : HTMLMediaElement(std::move(aNodeInfo)) {}

HTMLVideoElement::~HTMLVideoElement() = default;

void HTMLVideoElement::UpdateMediaSize(const nsIntSize& aSize) {
  HTMLMediaElement::UpdateMediaSize(aSize);
}

Maybe<CSSIntSize> HTMLVideoElement::GetVideoSize() const {
  if (!mMediaInfo.HasVideo()) {
    return Nothing();
  }

  if (mDisableVideo) {
    return Nothing();
  }

  CSSIntSize size;
  switch (mMediaInfo.mVideo.mRotation) {
    case VideoRotation::kDegree_90:
    case VideoRotation::kDegree_270: {
      size.width = mMediaInfo.mVideo.mDisplay.height;
      size.height = mMediaInfo.mVideo.mDisplay.width;
      break;
    }
    case VideoRotation::kDegree_0:
    case VideoRotation::kDegree_180:
    default: {
      size.height = mMediaInfo.mVideo.mDisplay.height;
      size.width = mMediaInfo.mVideo.mDisplay.width;
      break;
    }
  }
  return Some(size);
}

void HTMLVideoElement::Invalidate(ImageSizeChanged aImageSizeChanged,
                                  const Maybe<nsIntSize>& aNewIntrinsicSize,
                                  ForceInvalidate aForceInvalidate) {
  HTMLMediaElement::Invalidate(aImageSizeChanged, aNewIntrinsicSize,
                               aForceInvalidate);
  if (mVideoFrameRequestManager.IsEmpty()) {
    return;
  }

  if (RefPtr<ImageContainer> imageContainer = GetImageContainer()) {
    if (imageContainer->HasCurrentImage()) {
      OwnerDoc()->ScheduleVideoFrameCallbacks(this);
    }
  }
}

bool HTMLVideoElement::ParseAttribute(int32_t aNamespaceID, nsAtom* aAttribute,
                                      const nsAString& aValue,
                                      nsIPrincipal* aMaybeScriptedPrincipal,
                                      nsAttrValue& aResult) {
  if (aAttribute == nsGkAtoms::width || aAttribute == nsGkAtoms::height) {
    return aResult.ParseHTMLDimension(aValue);
  }

  return HTMLMediaElement::ParseAttribute(aNamespaceID, aAttribute, aValue,
                                          aMaybeScriptedPrincipal, aResult);
}

void HTMLVideoElement::MapAttributesIntoRule(
    MappedDeclarationsBuilder& aBuilder) {
  MapImageSizeAttributesInto(aBuilder, MapAspectRatio::Yes);
  MapCommonAttributesInto(aBuilder);
}

NS_IMETHODIMP_(bool)
HTMLVideoElement::IsAttributeMapped(const nsAtom* aAttribute) const {
  static const MappedAttributeEntry attributes[] = {
      {nsGkAtoms::width}, {nsGkAtoms::height}, {nullptr}};

  static const MappedAttributeEntry* const map[] = {attributes,
                                                    sCommonAttributeMap};

  return FindAttributeDependence(aAttribute, map);
}

nsMapRuleToAttributesFunc HTMLVideoElement::GetAttributeMappingFunction()
    const {
  return &MapAttributesIntoRule;
}

nsresult HTMLVideoElement::SetAcceptHeader(nsIHttpChannel* aChannel) {
  nsAutoCString value(
      "video/webm,"
      "video/*;q=0.9,"
      "application/ogg;q=0.7,"
      "audio/*;q=0.6,*/*;q=0.5");

  return aChannel->SetRequestHeader("Accept"_ns, value, false);
}

bool HTMLVideoElement::IsInteractiveHTMLContent() const {
  return HasAttr(nsGkAtoms::controls) ||
         HTMLMediaElement::IsInteractiveHTMLContent();
}

gfx::IntSize HTMLVideoElement::GetVideoIntrinsicDimensions() {
  const auto& sz = mMediaInfo.mVideo.mDisplay;

  return ToMaybeRef(mVideoFrameContainer.get())
      .map([&](auto& aVFC) { return aVFC.CurrentIntrinsicSize().valueOr(sz); })
      .valueOr(sz);
}

uint32_t HTMLVideoElement::VideoWidth() {
  if (!HasVideo()) {
    return 0;
  }
  gfx::IntSize size = GetVideoIntrinsicDimensions();
  if (mMediaInfo.mVideo.mRotation == VideoRotation::kDegree_90 ||
      mMediaInfo.mVideo.mRotation == VideoRotation::kDegree_270) {
    return size.height;
  }
  return size.width;
}

uint32_t HTMLVideoElement::VideoHeight() {
  if (!HasVideo()) {
    return 0;
  }
  gfx::IntSize size = GetVideoIntrinsicDimensions();
  if (mMediaInfo.mVideo.mRotation == VideoRotation::kDegree_90 ||
      mMediaInfo.mVideo.mRotation == VideoRotation::kDegree_270) {
    return size.width;
  }
  return size.height;
}

uint32_t HTMLVideoElement::MozParsedFrames() const {
  MOZ_ASSERT(NS_IsMainThread(), "Should be on main thread.");
  if (!IsVideoStatsEnabled()) {
    return 0;
  }

  if (OwnerDoc()->ShouldResistFingerprinting(
          RFPTarget::VideoElementMozFrames)) {
    return nsRFPService::GetSpoofedTotalFrames(TotalPlayTime());
  }

  return mDecoder ? mDecoder->GetFrameStatistics().GetParsedFrames() : 0;
}

uint32_t HTMLVideoElement::MozDecodedFrames() const {
  MOZ_ASSERT(NS_IsMainThread(), "Should be on main thread.");
  if (!IsVideoStatsEnabled()) {
    return 0;
  }

  if (OwnerDoc()->ShouldResistFingerprinting(
          RFPTarget::VideoElementMozFrames)) {
    return nsRFPService::GetSpoofedTotalFrames(TotalPlayTime());
  }

  return mDecoder ? mDecoder->GetFrameStatistics().GetDecodedFrames() : 0;
}

uint32_t HTMLVideoElement::MozPresentedFrames() {
  MOZ_ASSERT(NS_IsMainThread(), "Should be on main thread.");
  if (!IsVideoStatsEnabled()) {
    return 0;
  }

  if (OwnerDoc()->ShouldResistFingerprinting(
          RFPTarget::VideoElementMozFrames)) {
    return nsRFPService::GetSpoofedPresentedFrames(TotalPlayTime(),
                                                   VideoWidth(), VideoHeight());
  }

  return mDecoder ? mDecoder->GetFrameStatistics().GetPresentedFrames() : 0;
}

uint32_t HTMLVideoElement::MozPaintedFrames() {
  MOZ_ASSERT(NS_IsMainThread(), "Should be on main thread.");
  if (!IsVideoStatsEnabled()) {
    return 0;
  }

  if (OwnerDoc()->ShouldResistFingerprinting(
          RFPTarget::VideoElementMozFrames)) {
    return nsRFPService::GetSpoofedPresentedFrames(TotalPlayTime(),
                                                   VideoWidth(), VideoHeight());
  }

  layers::ImageContainer* container = GetImageContainer();
  return container ? container->GetPaintCount() : 0;
}

double HTMLVideoElement::MozFrameDelay() {
  MOZ_ASSERT(NS_IsMainThread(), "Should be on main thread.");

  if (!IsVideoStatsEnabled() || OwnerDoc()->ShouldResistFingerprinting(
                                    RFPTarget::VideoElementMozFrameDelay)) {
    return 0.0;
  }

  VideoFrameContainer* container = GetVideoFrameContainer();
  return container ? std::max(0.0, container->GetFrameDelay()) : 0.0;
}

bool HTMLVideoElement::MozHasAudio() const {
  MOZ_ASSERT(NS_IsMainThread(), "Should be on main thread.");
  return HasAudio();
}

JSObject* HTMLVideoElement::WrapNode(JSContext* aCx,
                                     JS::Handle<JSObject*> aGivenProto) {
  return HTMLVideoElement_Binding::Wrap(aCx, this, aGivenProto);
}

already_AddRefed<VideoPlaybackQuality>
HTMLVideoElement::GetVideoPlaybackQuality() {
  DOMHighResTimeStamp creationTime = 0;
  uint32_t totalFrames = 0;
  uint32_t droppedFrames = 0;

  if (IsVideoStatsEnabled()) {
    if (nsPIDOMWindowInner* window = OwnerDoc()->GetInnerWindow()) {
      Performance* perf = window->GetPerformance();
      if (perf) {
        creationTime = perf->Now();
      }
    }

    if (mDecoder) {
      if (OwnerDoc()->ShouldResistFingerprinting(
              RFPTarget::VideoElementPlaybackQuality)) {
        totalFrames = nsRFPService::GetSpoofedTotalFrames(TotalPlayTime());
        droppedFrames = nsRFPService::GetSpoofedDroppedFrames(
            TotalPlayTime(), VideoWidth(), VideoHeight());
      } else {
        FrameStatistics* stats = &mDecoder->GetFrameStatistics();
        if (sizeof(totalFrames) >= sizeof(stats->GetParsedFrames())) {
          totalFrames = stats->GetTotalFrames();
          droppedFrames = stats->GetDroppedFrames();
        } else {
          uint64_t total = stats->GetTotalFrames();
          const auto maxNumber = std::numeric_limits<uint32_t>::max();
          if (total <= maxNumber) {
            totalFrames = uint32_t(total);
            droppedFrames = uint32_t(stats->GetDroppedFrames());
          } else {
            double ratio = double(maxNumber) / double(total);
            totalFrames = maxNumber;  
            droppedFrames = uint32_t(double(stats->GetDroppedFrames()) * ratio);
          }
        }
      }
      if (!StaticPrefs::media_video_dropped_frame_stats_enabled()) {
        droppedFrames = 0;
      }
    }
  }

  RefPtr<VideoPlaybackQuality> playbackQuality =
      new VideoPlaybackQuality(this, creationTime, totalFrames, droppedFrames);
  return playbackQuality.forget();
}

void HTMLVideoElement::WakeLockRelease() {
  HTMLMediaElement::WakeLockRelease();
  ReleaseVideoWakeLockIfExists();
}

void HTMLVideoElement::UpdateWakeLock() {
  HTMLMediaElement::UpdateWakeLock();
  if (!mPaused) {
    CreateVideoWakeLockIfNeeded();
  } else {
    ReleaseVideoWakeLockIfExists();
  }
}

bool HTMLVideoElement::ShouldCreateVideoWakeLock() const {
  if (!StaticPrefs::media_video_wakelock()) {
    return false;
  }
  return HasVideo() && (mSrcStream || HasAudio());
}

void HTMLVideoElement::CreateVideoWakeLockIfNeeded() {
  if (AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
    return;
  }
  if (!mScreenWakeLock && ShouldCreateVideoWakeLock()) {
    RefPtr<power::PowerManagerService> pmService =
        power::PowerManagerService::GetInstance();
    NS_ENSURE_TRUE_VOID(pmService);

    ErrorResult rv;
    mScreenWakeLock = pmService->NewWakeLock(u"video-playing"_ns,
                                             OwnerDoc()->GetInnerWindow(), rv);
  }
}

void HTMLVideoElement::ReleaseVideoWakeLockIfExists() {
  if (mScreenWakeLock) {
    ErrorResult rv;
    mScreenWakeLock->Unlock(rv);
    rv.SuppressException();
    mScreenWakeLock = nullptr;
    return;
  }
}

bool HTMLVideoElement::IsVideoStatsEnabled() {
  return StaticPrefs::media_video_stats_enabled();
}

double HTMLVideoElement::TotalPlayTime() const {
  double total = 0.0;

  if (mPlayed) {
    uint32_t timeRangeCount = mPlayed->Length();

    for (uint32_t i = 0; i < timeRangeCount; i++) {
      double begin = mPlayed->Start(i);
      double end = mPlayed->End(i);
      total += end - begin;
    }

    if (mCurrentPlayRangeStart != -1.0) {
      double now = CurrentTime();
      if (mCurrentPlayRangeStart != now) {
        total += now - mCurrentPlayRangeStart;
      }
    }
  }

  return total;
}

void HTMLVideoElement::OnVisibilityChange(Visibility aNewVisibility) {
  HTMLMediaElement::OnVisibilityChange(aNewVisibility);

  if (!HasAttr(nsGkAtoms::autoplay) || IsAudible()) {
    return;
  }

  if (aNewVisibility == Visibility::ApproximatelyVisible && mPaused &&
      IsEligibleForAutoplay() && AllowedToPlay()) {
    LOG("resume invisible paused autoplay video");
    RunAutoplay();
  }

  if (aNewVisibility == Visibility::ApproximatelyNonVisible &&
      mCanAutoplayFlag) {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        __func__, [self = RefPtr<HTMLMediaElement>(this), this] {
          if (mVisibilityState != Visibility::ApproximatelyNonVisible ||
              !mCanAutoplayFlag) {
            return;
          }
          LOG("pause non-audible autoplay video when it's invisible");
          PauseInternal();
          mCanAutoplayFlag = true;
        }));
    return;
  }
}

void HTMLVideoElement::ResetState() {
  HTMLMediaElement::ResetState();
  mLastPresentedFrameID = layers::kContainerFrameID_Invalid;
}

bool HTMLVideoElement::WillFireVideoFrameCallbacks(
    const TimeStamp& aNowTime, const Maybe<TimeStamp>& aNextTickTime,
    VideoFrameCallbackMetadata& aMd) {
  AutoTArray<ImageContainer::OwningImage, 4> images;
  if (RefPtr<layers::ImageContainer> container = GetImageContainer()) {
    container->GetCurrentImages(&images);
  }

  if (images.IsEmpty()) {
    return false;
  }

  const ImageContainer::OwningImage* selected = nullptr;
  bool composited = false;
  for (const auto& image : images) {
    if (image.mTimeStamp <= aNowTime) {
      selected = &image;
      composited = true;
    } else if (!aNextTickTime || image.mTimeStamp <= aNextTickTime.ref()) {
      selected = &image;
      composited = false;
    } else {
      break;
    }
  }

  if (!selected || selected->mFrameID == layers::kContainerFrameID_Invalid ||
      selected->mFrameID == mLastPresentedFrameID) {
    return false;
  }

  gfx::IntSize frameSize = selected->mImage->GetSize();
  if (NS_WARN_IF(frameSize.IsEmpty())) {
    return false;
  }

  if (composited) {
    aMd.mExpectedDisplayTime = aMd.mPresentationTime;
  }

  MOZ_ASSERT(!frameSize.IsEmpty());

  aMd.mWidth = frameSize.width;
  aMd.mHeight = frameSize.height;

  aMd.mMediaTime = selected->mMediaTime.IsValid()
                       ? selected->mMediaTime.ToSeconds()
                       : CurrentTime();

  if (selected->mProcessingDuration.IsValid()) {
    aMd.mProcessingDuration.Construct(
        selected->mProcessingDuration.ToBase(10000).ToSeconds());
  }


  mPresentedFrames +=
      selected->mFrameID > 1 && selected->mFrameID > mLastPresentedFrameID
          ? selected->mFrameID - mLastPresentedFrameID
          : 1;
  mLastPresentedFrameID = selected->mFrameID;

  aMd.mPresentedFrames = mPresentedFrames;

  NS_DispatchToMainThread(NewRunnableMethod(
      "HTMLVideoElement::FinishedVideoFrameRequestCallbacks", this,
      &HTMLVideoElement::FinishedVideoFrameRequestCallbacks));

  return true;
}

void HTMLVideoElement::FinishedVideoFrameRequestCallbacks() {
  if (!HasPendingCallbacks()) {
    NotifyDecoderActivityChanges();
  }
}

uint32_t HTMLVideoElement::RequestVideoFrameCallback(
    VideoFrameRequestCallback& aCallback, ErrorResult& aRv) {
  bool hasPending = HasPendingCallbacks();
  uint32_t handle = 0;
  aRv = mVideoFrameRequestManager.Schedule(aCallback, &handle);
  if (!hasPending && HasPendingCallbacks()) {
    NotifyDecoderActivityChanges();
  }
  return handle;
}

void HTMLVideoElement::CancelVideoFrameCallback(uint32_t aHandle) {
  if (mVideoFrameRequestManager.Cancel(aHandle) && !HasPendingCallbacks()) {
    NotifyDecoderActivityChanges();
  }
}

}  

#undef LOG
