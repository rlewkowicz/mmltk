/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDecodingTask.h"

#include "DecodePool.h"
#include "Decoder.h"
#include "RasterImage.h"
#include "SurfaceCache.h"
#include "mozilla/AppShutdown.h"
#include "nsThreadUtils.h"

namespace mozilla {

using gfx::IntRect;

namespace image {


void IDecodingTask::NotifyProgress(NotNull<RasterImage*> aImage,
                                   NotNull<Decoder*> aDecoder) {
  MOZ_ASSERT(aDecoder->HasProgress() && !aDecoder->IsMetadataDecode());

  Progress progress = aDecoder->TakeProgress();
  OrientedIntRect invalidRect = aDecoder->TakeInvalidRect();
  Maybe<uint32_t> frameCount = aDecoder->TakeCompleteFrameCount();
  DecoderFlags decoderFlags = aDecoder->GetDecoderFlags();
  SurfaceFlags surfaceFlags = aDecoder->GetSurfaceFlags();

  if (NS_IsMainThread() && !(decoderFlags & DecoderFlags::ASYNC_NOTIFY)) {
    aImage->NotifyProgress(progress, invalidRect, frameCount, decoderFlags,
                           surfaceFlags);
    return;
  }

  if (NS_WARN_IF(
          AppShutdown::IsInOrBeyond(ShutdownPhase::XPCOMShutdownThreads))) {
    return;
  }

  NotNull<RefPtr<RasterImage>> image = aImage;
  nsCOMPtr<nsIEventTarget> eventTarget = GetMainThreadSerialEventTarget();
  eventTarget->Dispatch(CreateRenderBlockingRunnable(NS_NewRunnableFunction(
                            "IDecodingTask::NotifyProgress",
                            [=]() -> void {
                              image->NotifyProgress(progress, invalidRect,
                                                    frameCount, decoderFlags,
                                                    surfaceFlags);
                            })),
                        NS_DISPATCH_NORMAL);
}

void IDecodingTask::NotifyDecodeComplete(NotNull<RasterImage*> aImage,
                                         NotNull<Decoder*> aDecoder) {
  MOZ_ASSERT(aDecoder->HasError() || !aDecoder->InFrame(),
             "Decode complete in the middle of a frame?");

  DecoderFinalStatus finalStatus = aDecoder->FinalStatus();
  ImageMetadata metadata = aDecoder->GetImageMetadata();
  Progress progress = aDecoder->TakeProgress();
  OrientedIntRect invalidRect = aDecoder->TakeInvalidRect();
  Maybe<uint32_t> frameCount = aDecoder->TakeCompleteFrameCount();
  DecoderFlags decoderFlags = aDecoder->GetDecoderFlags();
  SurfaceFlags surfaceFlags = aDecoder->GetSurfaceFlags();

  if (NS_IsMainThread() && !(decoderFlags & DecoderFlags::ASYNC_NOTIFY)) {
    aImage->NotifyDecodeComplete(finalStatus, metadata, progress, invalidRect,
                                 frameCount, decoderFlags, surfaceFlags);
    return;
  }

  if (NS_WARN_IF(
          AppShutdown::IsInOrBeyond(ShutdownPhase::XPCOMShutdownThreads))) {
    return;
  }

  NotNull<RefPtr<RasterImage>> image = aImage;
  nsCOMPtr<nsIEventTarget> eventTarget = GetMainThreadSerialEventTarget();
  eventTarget->Dispatch(
      CreateRenderBlockingRunnable(NS_NewRunnableFunction(
          "IDecodingTask::NotifyDecodeComplete",
          [image, finalStatus, metadata = std::move(metadata), progress,
           invalidRect, frameCount, decoderFlags, surfaceFlags]() -> void {
            image->NotifyDecodeComplete(finalStatus, metadata, progress,
                                        invalidRect, frameCount, decoderFlags,
                                        surfaceFlags);
          })),
      NS_DISPATCH_NORMAL);
}


void IDecodingTask::Resume() { DecodePool::Singleton()->AsyncRun(this); }


MetadataDecodingTask::MetadataDecodingTask(NotNull<Decoder*> aDecoder)
    : mMutex("mozilla::image::MetadataDecodingTask"), mDecoder(aDecoder) {
  MOZ_ASSERT(mDecoder->IsMetadataDecode(),
             "Use DecodingTask for non-metadata decodes");
}

void MetadataDecodingTask::Run() {
  MutexAutoLock lock(mMutex);

  LexerResult result = mDecoder->Decode(WrapNotNull(this));

  if (result.is<TerminalState>()) {
    NotifyDecodeComplete(mDecoder->GetImage(), mDecoder);
    return;  
  }

  if (result == LexerResult(Yield::NEED_MORE_DATA)) {
    return;
  }

  MOZ_ASSERT_UNREACHABLE("Metadata decode yielded for an unexpected reason");
}


AnonymousDecodingTask::AnonymousDecodingTask(NotNull<Decoder*> aDecoder,
                                             bool aResumable)
    : mDecoder(aDecoder), mResumable(aResumable) {}

void AnonymousDecodingTask::Run() {
  while (true) {
    LexerResult result = mDecoder->Decode(WrapNotNull(this));

    if (result.is<TerminalState>()) {
      return;  
    }

    if (result == LexerResult(Yield::NEED_MORE_DATA)) {
      return;
    }

    MOZ_ASSERT(result.is<Yield>());
  }
}

void AnonymousDecodingTask::Resume() {
  if (mResumable) {
    RefPtr<AnonymousDecodingTask> self(this);
    NS_DispatchToMainThread(
        NS_NewRunnableFunction("image::AnonymousDecodingTask::Resume",
                               [self]() -> void { self->Run(); }));
  }
}

}  
}  
