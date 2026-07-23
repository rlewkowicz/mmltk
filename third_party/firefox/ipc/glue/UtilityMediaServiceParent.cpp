/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "UtilityMediaServiceParent.h"
#include "nsDebugImpl.h"

#include "MediaCodecsSupport.h"
#include "mozilla/RemoteMediaManagerParent.h"

#include "mozilla/gfx/gfxVars.h"



#include "mozilla/ipc/UtilityProcessChild.h"
#include "mozilla/RemoteDecodeUtils.h"

namespace mozilla::ipc {

UtilityMediaServiceParent::UtilityMediaServiceParent(
    nsTArray<gfx::GfxVarUpdate>&& aUpdates)
    : mKind(GetCurrentUtilityProcessKind()),
      mUtilityMediaServiceParentStart(TimeStamp::Now()) {
  switch (mKind) {
    case UtilityProcessKind::GENERIC_UTILITY:
      break;
    default:
      nsDebugImpl::SetMultiprocessMode("Utility AudioDecoder");
      break;
  }
  gfx::gfxVars::Initialize();
  gfx::gfxVars::ApplyUpdate(aUpdates);
}

UtilityMediaServiceParent::~UtilityMediaServiceParent() {
  gfx::gfxVars::Shutdown();
}

void UtilityMediaServiceParent::GenericPreloadForSandbox() {
}

void UtilityMediaServiceParent::WMFPreloadForSandbox() {
}

void UtilityMediaServiceParent::Start(
    Endpoint<PUtilityMediaServiceParent>&& aEndpoint) {
  MOZ_ASSERT(NS_IsMainThread());

  DebugOnly<bool> ok = std::move(aEndpoint).Bind(this);
  MOZ_ASSERT(ok);


  auto supported = media::MCSInfo::GetSupportFromFactory();
  (void)SendUpdateMediaCodecsSupported(GetRemoteMediaInFromKind(mKind),
                                       supported);
}

mozilla::ipc::IPCResult
UtilityMediaServiceParent::RecvNewContentRemoteMediaManager(
    Endpoint<PRemoteMediaManagerParent>&& aEndpoint,
    const ContentParentId& aParentId) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!RemoteMediaManagerParent::CreateForContent(std::move(aEndpoint),
                                                  aParentId)) {
    return IPC_FAIL_NO_REASON(this);
  }
  return IPC_OK();
}

IPCResult UtilityMediaServiceParent::RecvUpdateVar(
    const nsTArray<GfxVarUpdate>& aUpdate) {
  gfx::gfxVars::ApplyUpdate(aUpdate);

  MOZ_ALWAYS_SUCCEEDS(NS_DispatchBackgroundTask(
      NS_NewRunnableFunction(
          "UtilityMediaServiceParent::RecvUpdateVar",
          [self = RefPtr{this}]() {
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "UtilityMediaServiceParent::UpdateMediaCodecsSupported",
                [self, supported = media::MCSInfo::GetSupportFromFactory(
                           true )]() {
                  (void)self->SendUpdateMediaCodecsSupported(
                      GetRemoteMediaInFromKind(self->mKind), supported);
                }));
          }),
      nsIEventTarget::DISPATCH_NORMAL));
  return IPC_OK();
}

}  
