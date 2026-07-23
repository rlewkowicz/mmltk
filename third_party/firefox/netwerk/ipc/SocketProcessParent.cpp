/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SocketProcessParent.h"
#include "SocketProcessLogging.h"

#include "AltServiceParent.h"
#include "SSLTokensCache.h"
#include "HttpTransactionParent.h"
#include "SocketProcessHost.h"
#include "TLSClientAuthCertSelection.h"
#include "mozilla/Atomics.h"
#include "mozilla/Components.h"
#include "mozilla/dom/MemoryReportRequest.h"
#include "mozilla/net/DNSRequestParent.h"
#include "mozilla/net/ProxyConfigLookupParent.h"
#include "mozilla/net/SocketProcessBackgroundParent.h"
#include "mozilla/RemoteLazyInputStreamParent.h"
#include "nsIConsoleService.h"
#include "nsIHttpActivityObserver.h"
#include "nsIObserverService.h"
#include "nsNSSCertificate.h"
#include "nsNSSComponent.h"
#include "nsIOService.h"
#include "mozilla/net/neqo_glue_ffi_generated.h"
#include "nsSocketTransportService2.h"
#include "nsHttpHandler.h"
#include "nsHttpConnectionInfo.h"
#include "secerr.h"

namespace mozilla {
namespace net {

static Atomic<SocketProcessParent*> sSocketProcessParent;

SocketProcessParent::SocketProcessParent(SocketProcessHost* aHost)
    : mHost(aHost) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mHost);

  MOZ_COUNT_CTOR(SocketProcessParent);
  sSocketProcessParent = this;
}

SocketProcessParent::~SocketProcessParent() {
  MOZ_COUNT_DTOR(SocketProcessParent);
  sSocketProcessParent = nullptr;
}

already_AddRefed<SocketProcessParent> SocketProcessParent::GetSingleton() {
  RefPtr<SocketProcessParent> parent(sSocketProcessParent);
  return parent.forget();
}

void SocketProcessParent::ActorDestroy(ActorDestroyReason aWhy) {

  if (mHost) {
    mHost->OnChannelClosed();
  }
}

bool SocketProcessParent::SendRequestMemoryReport(
    const uint32_t& aGeneration, const bool& aAnonymize,
    const bool& aMinimizeMemoryUsage,
    const Maybe<ipc::FileDescriptor>& aDMDFile) {
  mMemoryReportRequest = MakeUnique<dom::MemoryReportRequestHost>(aGeneration);

  PSocketProcessParent::SendRequestMemoryReport(aGeneration, aAnonymize,
                                                aMinimizeMemoryUsage, aDMDFile)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [](uint32_t aGeneration2) {
            MOZ_ASSERT(gIOService);
            if (!gIOService->SocketProcess()) {
              return;
            }
            SocketProcessParent* actor =
                gIOService->SocketProcess()->GetActor();
            if (!actor) {
              return;
            }
            if (actor->mMemoryReportRequest) {
              actor->mMemoryReportRequest->Finish(aGeneration2);
              actor->mMemoryReportRequest = nullptr;
            }
          },
          [](mozilla::ipc::ResponseRejectReason) {
            MOZ_ASSERT(gIOService);
            if (!gIOService->SocketProcess()) {
              return;
            }
            SocketProcessParent* actor =
                gIOService->SocketProcess()->GetActor();
            if (!actor) {
              return;
            }
            actor->mMemoryReportRequest = nullptr;
          });

  return true;
}

mozilla::ipc::IPCResult SocketProcessParent::RecvAddMemoryReport(
    const MemoryReport& aReport) {
  if (mMemoryReportRequest) {
    mMemoryReportRequest->RecvReport(aReport);
  }
  return IPC_OK();
}

already_AddRefed<PDNSRequestParent> SocketProcessParent::AllocPDNSRequestParent(
    const nsACString& aHost, const nsACString& aTrrServer, const int32_t& port,
    const uint16_t& aType, const OriginAttributes& aOriginAttributes,
    const nsIDNSService::DNSFlags& aFlags) {
  RefPtr<DNSRequestHandler> handler = new DNSRequestHandler();
  RefPtr<DNSRequestParent> actor = new DNSRequestParent(handler);
  return actor.forget();
}

mozilla::ipc::IPCResult SocketProcessParent::RecvPDNSRequestConstructor(
    PDNSRequestParent* aActor, const nsACString& aHost,
    const nsACString& aTrrServer, const int32_t& port, const uint16_t& aType,
    const OriginAttributes& aOriginAttributes,
    const nsIDNSService::DNSFlags& aFlags) {
  RefPtr<DNSRequestParent> actor = static_cast<DNSRequestParent*>(aActor);
  RefPtr<DNSRequestHandler> handler =
      actor->GetDNSRequest()->AsDNSRequestHandler();
  handler->DoAsyncResolve(aHost, aTrrServer, port, aType, aOriginAttributes,
                          aFlags);
  return IPC_OK();
}

mozilla::ipc::IPCResult SocketProcessParent::RecvObserveHttpActivity(
    const HttpActivityArgs& aArgs, const uint32_t& aActivityType,
    const uint32_t& aActivitySubtype, const PRTime& aTimestamp,
    const uint64_t& aExtraSizeData, const nsACString& aExtraStringData) {
  nsCOMPtr<nsIHttpActivityDistributor> activityDistributor =
      components::HttpActivityDistributor::Service();
  MOZ_ASSERT(activityDistributor);

  (void)activityDistributor->ObserveActivityWithArgs(
      aArgs, aActivityType, aActivitySubtype, aTimestamp, aExtraSizeData,
      aExtraStringData);
  return IPC_OK();
}

mozilla::ipc::IPCResult SocketProcessParent::RecvInitSocketBackground(
    Endpoint<PSocketProcessBackgroundParent>&& aEndpoint) {
  if (!aEndpoint.IsValid()) {
    return IPC_FAIL(this, "Invalid endpoint");
  }

  nsCOMPtr<nsISerialEventTarget> transportQueue;
  if (NS_FAILED(NS_CreateBackgroundTaskQueue("SocketBackgroundParentQueue",
                                             getter_AddRefs(transportQueue)))) {
    return IPC_FAIL(this, "NS_CreateBackgroundTaskQueue failed");
  }

  transportQueue->Dispatch(
      NS_NewRunnableFunction("BindSocketBackgroundParent",
                             [endpoint = std::move(aEndpoint)]() mutable {
                               RefPtr<SocketProcessBackgroundParent> parent =
                                   new SocketProcessBackgroundParent();
                               endpoint.Bind(parent);
                             }));
  return IPC_OK();
}

already_AddRefed<PAltServiceParent>
SocketProcessParent::AllocPAltServiceParent() {
  RefPtr<AltServiceParent> actor = new AltServiceParent();
  return actor.forget();
}

already_AddRefed<PProxyConfigLookupParent>
SocketProcessParent::AllocPProxyConfigLookupParent(
    nsIURI* aURI, const uint32_t& aProxyResolveFlags) {
  RefPtr<ProxyConfigLookupParent> actor =
      new ProxyConfigLookupParent(aURI, aProxyResolveFlags);
  return actor.forget();
}

mozilla::ipc::IPCResult SocketProcessParent::RecvPProxyConfigLookupConstructor(
    PProxyConfigLookupParent* aActor, nsIURI* aURI,
    const uint32_t& aProxyResolveFlags) {
  static_cast<ProxyConfigLookupParent*>(aActor)->DoProxyLookup();
  return IPC_OK();
}

class DeferredDeleteSocketProcessParent : public Runnable {
 public:
  explicit DeferredDeleteSocketProcessParent(
      RefPtr<SocketProcessParent>&& aParent)
      : Runnable("net::DeferredDeleteSocketProcessParent"),
        mParent(std::move(aParent)) {}

  NS_IMETHODIMP Run() override { return NS_OK; }

 private:
  RefPtr<SocketProcessParent> mParent;
};

void SocketProcessParent::Destroy(RefPtr<SocketProcessParent>&& aParent) {
  NS_DispatchToMainThread(
      new DeferredDeleteSocketProcessParent(std::move(aParent)));
}

mozilla::ipc::IPCResult SocketProcessParent::RecvExcludeHttp2OrHttp3(
    const HttpConnectionInfoCloneArgs& aArgs) {
  RefPtr<nsHttpConnectionInfo> cinfo =
      nsHttpConnectionInfo::DeserializeHttpConnectionInfoCloneArgs(aArgs);
  if (!cinfo) {
    MOZ_ASSERT(false, "failed to deserizlize http connection info");
    return IPC_OK();
  }

  if (cinfo->IsHttp3()) {
    gHttpHandler->ExcludeHttp3(cinfo);
  } else {
    gHttpHandler->ExcludeHttp2(cinfo);
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult SocketProcessParent::RecvOnConsoleMessage(
    const nsString& aMessage) {
  nsCOMPtr<nsIConsoleService> consoleService =
      do_GetService(NS_CONSOLESERVICE_CONTRACTID);
  if (consoleService) {
    consoleService->LogStringMessage(aMessage.get());
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult SocketProcessParent::RecvSSLTokensCacheData(
    ByteBuf&& aBuf) {
  SSLTokensCache::DeserializeFromIPCAsync(std::move(aBuf));
  return IPC_OK();
}


}  
}  
