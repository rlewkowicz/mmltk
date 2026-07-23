/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HttpLog.h"


#include "nsHttp.h"
#include "nsHttpHandler.h"

#include "ASpdySession.h"
#include "Http2Session.h"

#include "mozilla/StaticPrefs_network.h"

namespace mozilla {
namespace net {

ASpdySession* ASpdySession::NewSpdySession(net::SpdyVersion version,
                                           nsISocketTransport* aTransport,
                                           bool attemptingEarlyData) {
  MOZ_ASSERT(version == SpdyVersion::HTTP_2, "Unsupported spdy version");


  return Http2Session::CreateSession(aTransport, version, attemptingEarlyData);
}

SpdyInformation::SpdyInformation() {
  Version = SpdyVersion::HTTP_2;
  VersionString = "h2"_ns;
  ALPNCallbacks = Http2Session::ALPNCallback;
}

}  
}  
