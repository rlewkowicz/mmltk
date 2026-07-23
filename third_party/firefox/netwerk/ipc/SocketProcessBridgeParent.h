/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_SocketProcessBridgeParent_h
#define mozilla_net_SocketProcessBridgeParent_h

#include "mozilla/net/PSocketProcessBridgeParent.h"

namespace mozilla {
namespace net {

class SocketProcessBridgeParent final : public PSocketProcessBridgeParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(SocketProcessBridgeParent, final)

  explicit SocketProcessBridgeParent(ProcessId aId);

  mozilla::ipc::IPCResult RecvInitBackgroundDataBridge(
      Endpoint<PBackgroundDataBridgeParent>&& aEndpoint, uint64_t aChannelID);


  void ActorDestroy(ActorDestroyReason aReason) override;

 private:
  ~SocketProcessBridgeParent();

  nsCOMPtr<nsISerialEventTarget> mMediaTransportTaskQueue;
  ProcessId mId;
};

}  
}  

#endif  // mozilla_net_SocketProcessBridgeParent_h
