/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "RDDChild.h"

#include "VideoUtils.h"
#include "mozilla/RDDProcessManager.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/MemoryReportRequest.h"
#include "mozilla/gfx/GPUProcessManager.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/ipc/Endpoint.h"




#include "RDDProcessHost.h"

namespace mozilla {

using namespace layers;
using namespace gfx;

RDDChild::RDDChild(RDDProcessHost* aHost) : mHost(aHost) {}

RDDChild::~RDDChild() = default;

bool RDDChild::Init() {
  Maybe<FileDescriptor> brokerFd;


  nsTArray<GfxVarUpdate> updates = gfxVars::FetchNonDefaultVars();

  SendInit(updates, brokerFd);

  gfxVars::AddReceiver(this);

  return true;
}

bool RDDChild::SendRequestMemoryReport(const uint32_t& aGeneration,
                                       const bool& aAnonymize,
                                       const bool& aMinimizeMemoryUsage,
                                       const Maybe<FileDescriptor>& aDMDFile) {
  mMemoryReportRequest = MakeUnique<MemoryReportRequestHost>(aGeneration);

  PRDDChild::SendRequestMemoryReport(aGeneration, aAnonymize,
                                     aMinimizeMemoryUsage, aDMDFile)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [](uint32_t aGeneration2) {
            if (RDDProcessManager* rddpm = RDDProcessManager::Get()) {
              if (RDDChild* child = rddpm->GetRDDChild()) {
                if (child->mMemoryReportRequest) {
                  child->mMemoryReportRequest->Finish(aGeneration2);
                  child->mMemoryReportRequest = nullptr;
                }
              }
            }
          },
          [](mozilla::ipc::ResponseRejectReason) {
            if (RDDProcessManager* rddpm = RDDProcessManager::Get()) {
              if (RDDChild* child = rddpm->GetRDDChild()) {
                child->mMemoryReportRequest = nullptr;
              }
            }
          });

  return true;
}

void RDDChild::OnCompositorUnexpectedShutdown() {
  if (!CanSend()) {
    return;
  }

  if (RDDProcessManager* rddpm = RDDProcessManager::Get()) {
    if (auto* gpm = GPUProcessManager::Get()) {
      gpm->CreateRddVideoBridge(rddpm, this);
    }
  }
}

void RDDChild::OnVarChanged(const nsTArray<GfxVarUpdate>& aVar) {
  SendUpdateVar(aVar);
}

mozilla::ipc::IPCResult RDDChild::RecvAddMemoryReport(
    const MemoryReport& aReport) {
  if (mMemoryReportRequest) {
    mMemoryReportRequest->RecvReport(aReport);
  }
  return IPC_OK();
}


mozilla::ipc::IPCResult RDDChild::RecvUpdateMediaCodecsSupported(
    const media::MediaCodecsSupported& aSupported) {
  dom::ContentParent::BroadcastMediaCodecsSupportedUpdate(
      RemoteMediaIn::RddProcess, aSupported);
  return IPC_OK();
}

void RDDChild::ActorDestroy(ActorDestroyReason aWhy) {
  if (auto* gpm = gfx::GPUProcessManager::Get()) {
    gpm->RemoveListener(this);
  }

  gfxVars::RemoveReceiver(this);
  mHost->OnChannelClosed();
}

class DeferredDeleteRDDChild : public Runnable {
 public:
  explicit DeferredDeleteRDDChild(RefPtr<RDDChild>&& aChild)
      : Runnable("gfx::DeferredDeleteRDDChild"), mChild(std::move(aChild)) {}

  NS_IMETHODIMP Run() override { return NS_OK; }

 private:
  RefPtr<RDDChild> mChild;
};

void RDDChild::Destroy(RefPtr<RDDChild>&& aChild) {
  NS_DispatchToMainThread(new DeferredDeleteRDDChild(std::move(aChild)));
}

}  
