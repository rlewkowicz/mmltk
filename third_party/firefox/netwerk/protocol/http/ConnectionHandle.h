/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ConnectionHandle_h_
#define ConnectionHandle_h_

#include "nsAHttpConnection.h"
#include "HttpConnectionBase.h"

namespace mozilla {
namespace net {

class ConnectionHandle : public nsAHttpConnection {
 public:
  NS_INLINE_DECL_REFCOUNTING_INHERITED(ConnectionHandle, nsAHttpConnection)
  NS_DECL_NSAHTTPCONNECTION(mConn)

  explicit ConnectionHandle(HttpConnectionBase* conn) : mConn(conn) {}
  void Reset() { mConn = nullptr; }
  HttpConnectionBase* Conn() { return mConn.get(); }

 private:
  virtual ~ConnectionHandle();
  RefPtr<HttpConnectionBase> mConn;
};

}  
}  

#endif  // ConnectionHandle_h_
