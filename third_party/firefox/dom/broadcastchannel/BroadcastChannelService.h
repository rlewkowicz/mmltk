/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_dom_BroadcastChannelService_h)
#define mozilla_dom_BroadcastChannelService_h

#include "nsClassHashtable.h"
#include "nsHashKeys.h"
#include "nsISupportsImpl.h"


namespace mozilla::dom {

class BroadcastChannelParent;
class SharedMessageBody;

class BroadcastChannelService final {
 public:
  NS_INLINE_DECL_REFCOUNTING(BroadcastChannelService)

  static already_AddRefed<BroadcastChannelService> GetOrCreate();

  void RegisterActor(BroadcastChannelParent* aParent,
                     const nsAString& aOriginChannelKey);
  void UnregisterActor(BroadcastChannelParent* aParent,
                       const nsAString& aOriginChannelKey);

  void PostMessage(BroadcastChannelParent* aParent,
                   NotNull<SharedMessageBody*> aData,
                   const nsAString& aOriginChannelKey);

 private:
  BroadcastChannelService();
  ~BroadcastChannelService();

  nsClassHashtable<nsStringHashKey, nsTArray<BroadcastChannelParent*>> mAgents;
};

}  

#endif
