/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "UtilityMediaServiceChild.h"

#include "base/basictypes.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/gfx/gfxVars.h"

namespace mozilla::ipc {

NS_IMETHODIMP UtilityMediaServiceChildShutdownObserver::Observe(
    nsISupports* aSubject, const char* aTopic, const char16_t* aData) {
  MOZ_ASSERT(strcmp(aTopic, "ipc:utility-shutdown") == 0);

  nsCOMPtr<nsIObserverService> observerService = services::GetObserverService();
  if (observerService) {
    observerService->RemoveObserver(this, "ipc:utility-shutdown");
  }

  UtilityMediaServiceChild::Shutdown(mKind);
  return NS_OK;
}

NS_IMPL_ISUPPORTS(UtilityMediaServiceChildShutdownObserver, nsIObserver);

static EnumeratedArray<UtilityProcessKind, StaticRefPtr<UtilityMediaServiceChild>,
                       size_t(UtilityProcessKind::COUNT)>
    sAudioDecoderChilds;

UtilityMediaServiceChild::UtilityMediaServiceChild(UtilityProcessKind aKind)
    : mKind(aKind), mAudioDecoderChildStart(TimeStamp::Now()) {
  MOZ_ASSERT(NS_IsMainThread());
  nsCOMPtr<nsIObserverService> observerService = services::GetObserverService();
  if (observerService) {
    auto* obs = new UtilityMediaServiceChildShutdownObserver(aKind);
    observerService->AddObserver(obs, "ipc:utility-shutdown", false);
  }
}

nsresult UtilityMediaServiceChild::BindToUtilityProcess(
    RefPtr<UtilityProcessParent> aUtilityParent) {
  Endpoint<PUtilityMediaServiceChild> utilityMediaServiceChildEnd;
  Endpoint<PUtilityMediaServiceParent> utilityMediaServiceParentEnd;
  nsresult rv = PUtilityMediaService::CreateEndpoints(
      aUtilityParent->OtherEndpointProcInfo(), EndpointProcInfo::Current(),
      &utilityMediaServiceParentEnd, &utilityMediaServiceChildEnd);

  if (NS_FAILED(rv)) {
    MOZ_ASSERT(false, "Protocol endpoints failure");
    return NS_ERROR_FAILURE;
  }

  nsTArray<gfx::GfxVarUpdate> updates = gfx::gfxVars::FetchNonDefaultVars();
  if (!aUtilityParent->SendStartUtilityMediaService(
          std::move(utilityMediaServiceParentEnd), std::move(updates))) {
    MOZ_ASSERT(false, "StartUtilityMediaService service failure");
    return NS_ERROR_FAILURE;
  }

  Bind(std::move(utilityMediaServiceChildEnd));

  return NS_OK;
}

void UtilityMediaServiceChild::ActorDestroy(ActorDestroyReason aReason) {
  MOZ_ASSERT(NS_IsMainThread());
  gfx::gfxVars::RemoveReceiver(this);
  Shutdown(mKind);
}

void UtilityMediaServiceChild::Bind(
    Endpoint<PUtilityMediaServiceChild>&& aEndpoint) {
  MOZ_ASSERT(NS_IsMainThread());
  if (NS_WARN_IF(!aEndpoint.Bind(this))) {
    MOZ_ASSERT_UNREACHABLE("Failed to bind UtilityMediaServiceChild!");
    return;
  }
  gfx::gfxVars::AddReceiver(this);
}

void UtilityMediaServiceChild::Shutdown(UtilityProcessKind aKind) {
  sAudioDecoderChilds[aKind] = nullptr;
}

RefPtr<UtilityMediaServiceChild> UtilityMediaServiceChild::GetSingleton(
    UtilityProcessKind aKind) {
  MOZ_ASSERT(NS_IsMainThread());
  bool shutdown = AppShutdown::IsInOrBeyond(ShutdownPhase::XPCOMWillShutdown);
  if (!sAudioDecoderChilds[aKind] && !shutdown) {
    sAudioDecoderChilds[aKind] = new UtilityMediaServiceChild(aKind);
  }
  return sAudioDecoderChilds[aKind];
}

mozilla::ipc::IPCResult
UtilityMediaServiceChild::RecvUpdateMediaCodecsSupported(
    const RemoteMediaIn& aLocation,
    const media::MediaCodecsSupported& aSupported) {
  dom::ContentParent::BroadcastMediaCodecsSupportedUpdate(aLocation,
                                                          aSupported);
  return IPC_OK();
}

void UtilityMediaServiceChild::OnVarChanged(
    const nsTArray<gfx::GfxVarUpdate>& aVar) {
  SendUpdateVar(aVar);
}

}  
