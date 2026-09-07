/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsIDeviceContextSpec.h"

#include "gfxPoint.h"
#include "mozilla/gfx/PrintPromise.h"
#include "nsError.h"
#include "nsIPrintSettings.h"

#include "mozilla/Components.h"
#include "mozilla/TaskQueue.h"

using mozilla::MakeRefPtr;
using mozilla::gfx::PrintEndDocumentPromise;


float nsIDeviceContextSpec::GetPrintingScale() {

  return 72.0f / GetDPI();
}

gfxPoint nsIDeviceContextSpec::GetPrintingTranslate() {
  return gfxPoint(0, 0);
}

RefPtr<PrintEndDocumentPromise>
nsIDeviceContextSpec::EndDocumentPromiseFromResult(
    nsresult aResult, mozilla::StaticString aSite) {
  return NS_SUCCEEDED(aResult)
             ? PrintEndDocumentPromise::CreateAndResolve(true, aSite)
             : PrintEndDocumentPromise::CreateAndReject(aResult, aSite);
}

RefPtr<PrintEndDocumentPromise> nsIDeviceContextSpec::EndDocumentAsync(
    const char* aCallSite, AsyncEndDocumentFunction aFunction) {
  auto promise =
      MakeRefPtr<PrintEndDocumentPromise::Private>("PrintEndDocumentPromise");

  NS_DispatchBackgroundTask(
      NS_NewRunnableFunction(
          "EndDocumentAsync",
          [promise, function = std::move(aFunction)]() mutable {
            const auto result = function();
            if (NS_SUCCEEDED(result)) {
              promise->Resolve(true, __func__);
            } else {
              promise->Reject(result, __func__);
            }
          }),
      NS_DISPATCH_EVENT_MAY_BLOCK);

  return promise;
}
