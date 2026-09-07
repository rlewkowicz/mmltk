/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "RemoteAudioDecoder.h"

#include "MediaDataDecoderProxy.h"
#include "PDMFactory.h"
#include "RemoteMediaManagerChild.h"
#include "RemoteMediaManagerParent.h"
#include "mozilla/StaticPrefs_media.h"

namespace mozilla {

RemoteAudioDecoderChild::RemoteAudioDecoderChild(RemoteMediaIn aLocation)
    : RemoteDecoderChild(aLocation) {}

MediaResult RemoteAudioDecoderChild::ProcessOutput(
    DecodedOutputIPDL&& aDecodedData) {
  AssertOnManagerThread();

  MOZ_ASSERT(aDecodedData.type() == DecodedOutputIPDL::TArrayOfRemoteAudioData);
  RefPtr<ArrayOfRemoteAudioData> arrayData =
      aDecodedData.get_ArrayOfRemoteAudioData();

  for (size_t i = 0; i < arrayData->Count(); i++) {
    RefPtr<AudioData> data = arrayData->ElementAt(i);
    if (!data) {
      return MediaResult(NS_ERROR_OUT_OF_MEMORY, __func__);
    }
    mDecodedData.AppendElement(data);
  }
  return NS_OK;
}

MediaResult RemoteAudioDecoderChild::InitIPDL(
    const AudioInfo& aAudioInfo,
    const CreateDecoderParams::OptionSet& aOptions) {
  RefPtr<RemoteMediaManagerChild> manager =
      RemoteMediaManagerChild::GetSingleton(mLocation);

  if (!manager) {
    return MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                       RESULT_DETAIL("RemoteMediaManager is not available."));
  }

  if (!manager->CanSend()) {
    return MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                       RESULT_DETAIL("RemoteMediaManager unable to send."));
  }

  if (!manager->SendPRemoteDecoderConstructor(this, aAudioInfo, aOptions,
                                              Nothing(), Nothing())) {
    return MediaResult(
        NS_ERROR_DOM_MEDIA_FATAL_ERR,
        RESULT_DETAIL("RemoteMediaManager unable to construct."));
  }

  return NS_OK;
}

RemoteAudioDecoderParent::RemoteAudioDecoderParent(
    RemoteMediaManagerParent* aParent, const AudioInfo& aAudioInfo,
    const CreateDecoderParams::OptionSet& aOptions,
    nsISerialEventTarget* aManagerThread, TaskQueue* aDecodeTaskQueue)
    : RemoteDecoderParent(aParent, aOptions, aManagerThread, aDecodeTaskQueue,
                          Nothing()),
      mAudioInfo(aAudioInfo) {}

IPCResult RemoteAudioDecoderParent::RecvConstruct(
    ConstructResolver&& aResolver) {
  if (mDecoder || mShutdown) {
    aResolver(MediaResult(NS_ERROR_ALREADY_INITIALIZED, __func__));
    return IPC_OK();
  }

  auto params = CreateDecoderParams{
      mAudioInfo, mOptions, CreateDecoderParams::WrapperSet({}),
      mTrackingId};

  mParent->EnsurePDMFactory().CreateDecoder(params)->Then(
      GetCurrentSerialEventTarget(), __func__,
      [resolver = std::move(aResolver), self = RefPtr{this}](
          PlatformDecoderModule::CreateDecoderPromise::ResolveOrRejectValue&&
              aValue) {
        if (aValue.IsReject()) {
          resolver(aValue.RejectValue());
          return;
        }
        MOZ_ASSERT(aValue.ResolveValue());
        if (self->mDecoder || self->mShutdown) {
          aValue.ResolveValue()->Shutdown();
          resolver(MediaResult(NS_ERROR_ALREADY_INITIALIZED, __func__));
          return;
        }
        self->mDecoder =
            new MediaDataDecoderProxy(aValue.ResolveValue().forget(),
                                      do_AddRef(self->mDecodeTaskQueue.get()));
        resolver(NS_OK);
      });

  return IPC_OK();
}

MediaResult RemoteAudioDecoderParent::ProcessDecodedData(
    MediaDataDecoder::DecodedData&& aData, DecodedOutputIPDL& aDecodedData) {
  MOZ_ASSERT(OnManagerThread());

  nsTArray<RefPtr<AudioData>> data(aData.Length());
  for (auto&& element : aData) {
    MOZ_ASSERT(element->mType == MediaData::Type::AUDIO_DATA,
               "Can only decode audio using RemoteAudioDecoderParent!");
    AudioData* audio = static_cast<AudioData*>(element.get());
    data.AppendElement(audio);
  }
  auto array = MakeRefPtr<ArrayOfRemoteAudioData>();
  if (!array->Fill(std::move(data),
                   [&](size_t aSize) { return AllocateBuffer(aSize); })) {
    return MediaResult(
        NS_ERROR_OUT_OF_MEMORY,
        "Failed in RemoteAudioDecoderParent::ProcessDecodedData");
  }
  aDecodedData = std::move(array);
  return NS_OK;
}

}  
