/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _include_ipc_glue_UtilityMediaServiceChild_h_
#define _include_ipc_glue_UtilityMediaServiceChild_h_

#include "mozilla/ProcInfo.h"
#include "mozilla/RefPtr.h"

#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/UtilityProcessParent.h"
#include "mozilla/ipc/UtilityProcessTypes.h"
#include "mozilla/ipc/UtilityMediaService.h"
#include "mozilla/ipc/PUtilityMediaServiceChild.h"
#include "mozilla/gfx/gfxVarReceiver.h"

#include "PDMFactory.h"

namespace mozilla::ipc {

class UtilityMediaServiceChildShutdownObserver : public nsIObserver {
 public:
  explicit UtilityMediaServiceChildShutdownObserver(UtilityProcessKind aKind)
      : mKind(aKind) {};

  NS_DECL_ISUPPORTS

  NS_IMETHOD Observe(nsISupports* aSubject, const char* aTopic,
                     const char16_t* aData) override;

 private:
  virtual ~UtilityMediaServiceChildShutdownObserver() = default;

  const UtilityProcessKind mKind;
};

class UtilityMediaServiceChild final : public PUtilityMediaServiceChild,
                                       public gfx::gfxVarReceiver {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(UtilityMediaServiceChild, override);
  mozilla::ipc::IPCResult RecvUpdateMediaCodecsSupported(
      const RemoteMediaIn& aLocation,
      const media::MediaCodecsSupported& aSupported);

  UtilityActorName GetActorName() { return GetAudioActorName(mKind); }

  nsresult BindToUtilityProcess(RefPtr<UtilityProcessParent> aUtilityParent);

  void ActorDestroy(ActorDestroyReason aReason) override;

  void Bind(Endpoint<PUtilityMediaServiceChild>&& aEndpoint);

  static void Shutdown(UtilityProcessKind aKind);

  static RefPtr<UtilityMediaServiceChild> GetSingleton(UtilityProcessKind aKind);

  void OnVarChanged(const nsTArray<gfx::GfxVarUpdate>& aVar) override;

 private:
  explicit UtilityMediaServiceChild(UtilityProcessKind aKind);
  ~UtilityMediaServiceChild() = default;

  const UtilityProcessKind mKind;

  TimeStamp mAudioDecoderChildStart;
};

}  

#endif  // _include_ipc_glue_UtilityMediaServiceChild_h_
