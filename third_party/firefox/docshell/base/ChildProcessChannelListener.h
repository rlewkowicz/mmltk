/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ChildProcessChannelListener_h
#define mozilla_dom_ChildProcessChannelListener_h

#include <functional>

#include "mozilla/net/NeckoChannelParams.h"
#include "nsDOMNavigationTiming.h"
#include "nsTHashMap.h"
#include "nsIChannel.h"
#include "mozilla/ipc/BackgroundUtils.h"

namespace mozilla::dom {

class ChildProcessChannelListener final {
  NS_INLINE_DECL_REFCOUNTING(ChildProcessChannelListener)

  using Resolver = std::function<void(const nsresult&)>;
  using Callback =
      std::function<nsresult(nsDocShellLoadState*, nsDOMNavigationTiming*)>;

  void RegisterCallback(uint64_t aIdentifier, Callback&& aCallback);

  void OnChannelReady(nsDocShellLoadState* aLoadState, uint64_t aIdentifier,
                      nsDOMNavigationTiming* aTiming, Resolver&& aResolver);

  static already_AddRefed<ChildProcessChannelListener> GetSingleton();

 private:
  ChildProcessChannelListener() = default;
  ~ChildProcessChannelListener();
  struct CallbackArgs {
    RefPtr<nsDocShellLoadState> mLoadState;
    RefPtr<nsDOMNavigationTiming> mTiming;
    Resolver mResolver;
  };

  nsTHashMap<NoMemMoveKey<nsUint64HashKey>, Callback> mCallbacks;
  nsTHashMap<NoMemMoveKey<nsUint64HashKey>, CallbackArgs> mChannelArgs;
};

}  

#endif  // !defined(mozilla_dom_ChildProcessChannelListener_h)
