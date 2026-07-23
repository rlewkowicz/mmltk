/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/ChildProcessChannelListener.h"

#include "mozilla/ipc/Endpoint.h"
#include "nsDocShellLoadState.h"

namespace mozilla {
namespace dom {

static StaticRefPtr<ChildProcessChannelListener> sCPCLSingleton;

void ChildProcessChannelListener::RegisterCallback(uint64_t aIdentifier,
                                                   Callback&& aCallback) {
  if (auto args = mChannelArgs.Extract(aIdentifier)) {
    nsresult rv = aCallback(args->mLoadState, args->mTiming);
    args->mResolver(rv);
  } else {
    mCallbacks.InsertOrUpdate(aIdentifier, std::move(aCallback));
  }
}

void ChildProcessChannelListener::OnChannelReady(
    nsDocShellLoadState* aLoadState, uint64_t aIdentifier,
    nsDOMNavigationTiming* aTiming, Resolver&& aResolver) {
  if (auto callback = mCallbacks.Extract(aIdentifier)) {
    nsresult rv = (*callback)(aLoadState, aTiming);
    aResolver(rv);
  } else {
    mChannelArgs.InsertOrUpdate(
        aIdentifier,
        CallbackArgs{aLoadState, aTiming, std::move(aResolver)});
  }
}

ChildProcessChannelListener::~ChildProcessChannelListener() {
  for (const auto& args : mChannelArgs.Values()) {
    args.mResolver(NS_ERROR_FAILURE);
  }
}

already_AddRefed<ChildProcessChannelListener>
ChildProcessChannelListener::GetSingleton() {
  if (!sCPCLSingleton) {
    sCPCLSingleton = new ChildProcessChannelListener();
    ClearOnShutdown(&sCPCLSingleton);
  }
  RefPtr<ChildProcessChannelListener> cpcl = sCPCLSingleton;
  return cpcl.forget();
}

}  
}  
