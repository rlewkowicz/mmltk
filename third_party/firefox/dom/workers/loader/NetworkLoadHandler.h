/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_NetworkLoadHandler_h_
#define mozilla_dom_workers_NetworkLoadHandler_h_

#include "mozilla/dom/ScriptLoadHandler.h"
#include "mozilla/dom/WorkerLoadContext.h"
#include "mozilla/dom/WorkerRef.h"
#include "nsIStreamLoader.h"

namespace mozilla::dom::workerinternals::loader {

class WorkerScriptLoader;


class NetworkLoadHandler final : public nsIStreamLoaderObserver,
                                 public nsIRequestObserver {
 public:
  NS_DECL_ISUPPORTS

  NetworkLoadHandler(WorkerScriptLoader* aLoader,
                     ThreadSafeRequestHandle* aRequestHandle);

  NS_IMETHOD
  OnStreamComplete(nsIStreamLoader* aLoader, nsISupports* aContext,
                   nsresult aStatus, uint32_t aStringLen,
                   const uint8_t* aString) override;

  nsresult DataReceivedFromNetwork(nsIStreamLoader* aLoader, nsresult aStatus,
                                   uint32_t aStringLen, const uint8_t* aString);

  NS_IMETHOD
  OnStartRequest(nsIRequest* aRequest) override;

  nsresult PrepareForRequest(nsIRequest* aRequest);

  NS_IMETHOD
  OnStopRequest(nsIRequest* aRequest, nsresult aStatusCode) override {
    return NS_OK;
  }

 private:
  ~NetworkLoadHandler() = default;

  RefPtr<WorkerScriptLoader> mLoader;
  UniquePtr<ScriptDecoder> mDecoder;
  RefPtr<ThreadSafeWorkerRef> mWorkerRef;
  RefPtr<ThreadSafeRequestHandle> mRequestHandle;
};

}  

#endif /* mozilla_dom_workers_NetworkLoadHandler_h_ */
