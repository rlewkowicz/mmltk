/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsNamedPipeIOLayer.h"
#include "nsSOCKSSocketProvider.h"
#include "nsSOCKSIOLayer.h"
#include "nsCOMPtr.h"
#include "nsError.h"

using mozilla::OriginAttributes;
using namespace mozilla::net;


NS_IMPL_ISUPPORTS(nsSOCKSSocketProvider, nsISocketProvider)


#if defined(XP_UNIX)

static PRFileDesc* OpenTCPSocket(int32_t family, nsIProxyInfo* proxy) {
  nsAutoCString proxyHost;
  proxy->GetHost(proxyHost);
  if (StringBeginsWith(proxyHost, "file://"_ns)) {
    family = AF_LOCAL;
  }

  return PR_OpenTCPSocket(family);
}
#else
static PRFileDesc* OpenTCPSocket(int32_t family, nsIProxyInfo*) {
  return PR_OpenTCPSocket(family);
}
#endif

NS_IMETHODIMP
nsSOCKSSocketProvider::NewSocket(int32_t family, const char* host, int32_t port,
                                 nsIProxyInfo* proxy,
                                 const OriginAttributes& originAttributes,
                                 uint32_t flags, uint32_t tlsFlags,
                                 PRFileDesc** result,
                                 nsITLSSocketControl** tlsSocketControl) {
  PRFileDesc* sock = OpenTCPSocket(family, proxy);
  if (!sock) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  nsresult rv = nsSOCKSIOLayerAddToSocket(family, host, port, proxy, mVersion,
                                          flags, tlsFlags, sock);
  if (NS_SUCCEEDED(rv)) {
    *result = sock;
    return NS_OK;
  }

  return NS_ERROR_SOCKET_CREATE_FAILED;
}

NS_IMETHODIMP
nsSOCKSSocketProvider::AddToSocket(int32_t family, const char* host,
                                   int32_t port, nsIProxyInfo* proxy,
                                   const OriginAttributes& originAttributes,
                                   uint32_t flags, uint32_t tlsFlags,
                                   PRFileDesc* sock,
                                   nsITLSSocketControl** tlsSocketControl) {
  nsresult rv = nsSOCKSIOLayerAddToSocket(family, host, port, proxy, mVersion,
                                          flags, tlsFlags, sock);

  if (NS_FAILED(rv)) rv = NS_ERROR_SOCKET_CREATE_FAILED;
  return rv;
}
