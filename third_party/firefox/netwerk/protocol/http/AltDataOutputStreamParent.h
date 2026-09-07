/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_AltDataOutputStreamParent_h
#define mozilla_net_AltDataOutputStreamParent_h

#include "mozilla/net/PAltDataOutputStreamParent.h"
#include "nsIOutputStream.h"

namespace mozilla {
namespace net {

class AltDataOutputStreamParent : public PAltDataOutputStreamParent,
                                  public nsISupports {
 public:
  NS_DECL_ISUPPORTS

  explicit AltDataOutputStreamParent(nsIOutputStream* aStream);

  mozilla::ipc::IPCResult RecvWriteData(const nsCString& data);
  mozilla::ipc::IPCResult RecvClose(const nsresult& aStatus);
  virtual void ActorDestroy(ActorDestroyReason aWhy) override;

  void SetError(nsresult status) { mStatus = status; }
  mozilla::ipc::IPCResult RecvDeleteSelf();

 private:
  virtual ~AltDataOutputStreamParent();
  nsCOMPtr<nsIOutputStream> mOutputStream;
  nsresult mStatus;
  bool mIPCOpen;
};

}  
}  

#endif  // mozilla_net_AltDataOutputStreamParent_h
