/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef MediaDecoderOwner_h_
#define MediaDecoderOwner_h_

#include "MediaInfo.h"
#include "MediaSegment.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/DefineEnum.h"
#include "mozilla/UniquePtr.h"
#include "nsSize.h"

namespace mozilla {

class VideoFrameContainer;
class MediaInfo;
class MediaResult;
enum class RFPTarget : uint64_t;

namespace dom {
class Document;
class HTMLMediaElement;
}  

class MediaDecoderOwner {
 public:
  virtual void DownloadProgressed() = 0;

  virtual void QueueEvent(const nsAString& aName) = 0;

  virtual void UpdateReadyState() = 0;

  virtual void MaybeQueueTimeupdateEvent() = 0;

  virtual bool GetPaused() = 0;

  virtual void MetadataLoaded(const MediaInfo* aInfo,
                              UniquePtr<const MetadataTags> aTags) = 0;

  virtual void FirstFrameLoaded() = 0;

  virtual void NetworkError(const MediaResult& aError) = 0;

  virtual void DecodeError(const MediaResult& aError) = 0;

  virtual void DecodeWarning(const MediaResult& aError) = 0;

  virtual bool HasError() const = 0;

  virtual void LoadAborted() = 0;

  virtual void PlaybackEnded() = 0;

  virtual void SeekStarted() = 0;

  virtual void SeekCompleted() = 0;

  virtual void UpdatePlayedRangesBeforeSeek(double aRangeEndTime) = 0;

  virtual void SeekAborted() = 0;

  virtual void DownloadSuspended() = 0;

  virtual void NotifySuspendedByCache(bool aSuspendedByCache) = 0;

  virtual void NotifyDecoderPrincipalChanged() = 0;

  MOZ_DEFINE_ENUM_WITH_TOSTRING_AT_CLASS_SCOPE(
      NextFrameStatus, (NEXT_FRAME_AVAILABLE, NEXT_FRAME_UNAVAILABLE_BUFFERING,
                        NEXT_FRAME_UNAVAILABLE_SEEKING, NEXT_FRAME_UNAVAILABLE,
                        NEXT_FRAME_UNINITIALIZED));

  virtual void SetAudibleState(bool aAudible) = 0;

  virtual void NotifyXPCOMShutdown() = 0;

  static AbstractThread* AbstractMainThread() {
    return AbstractThread::MainThread();
  }

  virtual dom::HTMLMediaElement* GetMediaElement() { return nullptr; }

  virtual VideoFrameContainer* GetVideoFrameContainer() { return nullptr; }

  virtual mozilla::dom::Document* GetDocument() const { return nullptr; }

  enum class ImageSizeChanged { No, Yes };
  enum class ForceInvalidate { No, Yes };
  virtual void Invalidate(ImageSizeChanged aImageSizeChanged,
                          const Maybe<nsIntSize>& aNewIntrinsicSize,
                          ForceInvalidate aForceInvalidate) {}

  virtual void PrincipalHandleChangedForVideoFrameContainer(
      VideoFrameContainer* aContainer,
      const PrincipalHandle& aNewPrincipalHandle) {}

  virtual void OnSecondaryVideoContainerInstalled(
      const RefPtr<VideoFrameContainer>& aSecondaryContainer) {}

  virtual bool IsActuallyInvisible() const = 0;

  virtual bool ShouldResistFingerprinting(RFPTarget aTarget) const = 0;

};

}  

#endif
