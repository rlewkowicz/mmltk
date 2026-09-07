/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ImageComposite.h"

#include "gfxPlatform.h"

namespace mozilla {

using namespace gfx;

namespace layers {

 const float ImageComposite::BIAS_TIME_MS = 1.0f;

ImageComposite::ImageComposite() = default;

ImageComposite::~ImageComposite() = default;

TimeStamp ImageComposite::GetBiasedTime(const TimeStamp& aInput) const {
  switch (mBias) {
    case ImageComposite::BIAS_NEGATIVE:
      return aInput - TimeDuration::FromMilliseconds(BIAS_TIME_MS);
    case ImageComposite::BIAS_POSITIVE:
      return aInput + TimeDuration::FromMilliseconds(BIAS_TIME_MS);
    default:
      return aInput;
  }
}

void ImageComposite::UpdateBias(size_t aImageIndex, bool aFrameChanged) {
  MOZ_ASSERT(aImageIndex < ImagesCount());

  TimeStamp compositionTime = GetCompositionTime();
  TimeStamp compositedImageTime = mImages[aImageIndex].mTimeStamp;
  TimeStamp nextImageTime = aImageIndex + 1 < ImagesCount()
                                ? mImages[aImageIndex + 1].mTimeStamp
                                : TimeStamp();

  if (compositedImageTime.IsNull()) {
    mBias = ImageComposite::BIAS_NONE;
    return;
  }
  TimeDuration threshold = TimeDuration::FromMilliseconds(1.5);
  if (compositionTime - compositedImageTime < threshold &&
      compositionTime - compositedImageTime > -threshold) {
    mBias = ImageComposite::BIAS_NEGATIVE;
    return;
  }
  if (!nextImageTime.IsNull() && nextImageTime - compositionTime < threshold &&
      nextImageTime - compositionTime > -threshold) {
    mBias = ImageComposite::BIAS_POSITIVE;
    return;
  }
  if (aFrameChanged) {
    mBias = ImageComposite::BIAS_NONE;
  }
}

int ImageComposite::ChooseImageIndex() {
  if (mImages.IsEmpty()) {
    return -1;
  }

  TimeStamp compositionTime = GetCompositionTime();
  auto compositionOpportunityId = GetCompositionOpportunityId();
  if (compositionTime &&
      compositionOpportunityId != mLastChooseImageIndexComposition) {
    uint32_t imageIndex = 0;
    while (imageIndex + 1 < mImages.Length() &&
           mImages[imageIndex + 1].mTextureHost->IsValid() &&
           GetBiasedTime(mImages[imageIndex + 1].mTimeStamp) <=
               compositionTime) {
      ++imageIndex;
    }

    if (!mImages[imageIndex].mTextureHost->IsValid()) {
      return -1;
    }

    bool wasVisibleAtPreviousComposition =
        compositionOpportunityId == mLastChooseImageIndexComposition.Next();

    bool frameChanged =
        UpdateCompositedFrame(imageIndex, wasVisibleAtPreviousComposition);
    UpdateBias(imageIndex, frameChanged);

    mLastChooseImageIndexComposition = compositionOpportunityId;

    return imageIndex;
  }

  for (uint32_t i = 0; i < mImages.Length(); ++i) {
    if (mImages[i].mFrameID == mLastFrameID &&
        mImages[i].mProducerID == mLastProducerID) {
      return i;
    }
  }

  return 0;
}

const ImageComposite::TimedImage* ImageComposite::ChooseImage() {
  int index = ChooseImageIndex();
  return index >= 0 ? &mImages[index] : nullptr;
}

void ImageComposite::RemoveImagesWithTextureHost(TextureHost* aTexture) {
  for (int32_t i = mImages.Length() - 1; i >= 0; --i) {
    if (mImages[i].mTextureHost == aTexture) {
      mImages.RemoveElementAt(i);
    }
  }
}

void ImageComposite::ClearImages() { mImages.Clear(); }

void ImageComposite::SetImages(nsTArray<TimedImage>&& aNewImages) {
  if (!aNewImages.IsEmpty()) {
    CountSkippedFrames(&aNewImages[0]);
  }
  mImages = std::move(aNewImages);
}

bool ImageComposite::UpdateCompositedFrame(
    int aImageIndex, bool aWasVisibleAtPreviousComposition) {
  MOZ_RELEASE_ASSERT(aImageIndex >= 0);
  MOZ_RELEASE_ASSERT(aImageIndex < static_cast<int>(mImages.Length()));
  const TimedImage& image = mImages[aImageIndex];

  auto compositionOpportunityId = GetCompositionOpportunityId();
  if (mLastFrameID == image.mFrameID && mLastProducerID == image.mProducerID) {
    return false;
  }

  CountSkippedFrames(&image);

  int32_t dropped = mSkippedFramesSinceLastComposite;
  mSkippedFramesSinceLastComposite = 0;

  if (!aWasVisibleAtPreviousComposition) {
    dropped = 0;
  }

  if (dropped > 0) {
    mDroppedFrames += dropped;
  }

  mLastFrameID = image.mFrameID;
  mLastProducerID = image.mProducerID;
  mLastFrameUpdateComposition = compositionOpportunityId;

  return true;
}

void ImageComposite::OnFinishRendering(int aImageIndex,
                                       const TimedImage* aImage,
                                       base::ProcessId aProcessId,
                                       const CompositableHandle& aHandle) {
  if (mLastFrameUpdateComposition != GetCompositionOpportunityId()) {
    return;
  }

  if (aHandle) {
    ImageCompositeNotificationInfo info;
    info.mImageBridgeProcessId = aProcessId;
    info.mNotification = ImageCompositeNotification(
        aHandle, aImage->mTimeStamp, GetCompositionTime(), mLastFrameID,
        mLastProducerID);
    AppendImageCompositeNotification(info);
  }
}

const ImageComposite::TimedImage* ImageComposite::GetImage(
    size_t aIndex) const {
  if (aIndex >= mImages.Length()) {
    return nullptr;
  }
  return &mImages[aIndex];
}

void ImageComposite::CountSkippedFrames(const TimedImage* aImage) {
  if (aImage->mProducerID != mLastProducerID) {
    return;
  }

  if (mImages.IsEmpty() || aImage->mFrameID <= mLastFrameID + 1) {
    return;
  }

  uint32_t targetFrameRate = gfxPlatform::TargetFrameRate();
  if (targetFrameRate == 0) {
    return;
  }

  double targetFrameDurationMS = 1000.0 / targetFrameRate;

  int32_t skipped = 0;
  for (size_t i = 0; i + 1 < mImages.Length(); i++) {
    const auto& img = mImages[i];
    if (img.mProducerID != aImage->mProducerID ||
        img.mFrameID <= mLastFrameID || img.mFrameID >= aImage->mFrameID) {
      continue;
    }

    const auto& next = mImages[i + 1];
    if (next.mProducerID != aImage->mProducerID) {
      continue;
    }

    MOZ_ASSERT(next.mFrameID > img.mFrameID);
    TimeDuration duration = next.mTimeStamp - img.mTimeStamp;
    if (floor(duration.ToMilliseconds()) >= floor(targetFrameDurationMS)) {
      skipped++;
    }
  }

  mSkippedFramesSinceLastComposite += skipped;
}

}  
}  
