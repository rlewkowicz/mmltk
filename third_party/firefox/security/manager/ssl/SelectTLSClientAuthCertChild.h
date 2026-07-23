/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SECURITY_MANAGER_SSL_SELECTTLSCLIENTAUTHCERTCHILD_H_
#define SECURITY_MANAGER_SSL_SELECTTLSCLIENTAUTHCERTCHILD_H_

#include "mozilla/psm/PSelectTLSClientAuthCertChild.h"
#include "TLSClientAuthCertSelection.h"

namespace mozilla {
namespace psm {

class SelectTLSClientAuthCertChild : public PSelectTLSClientAuthCertChild {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(SelectTLSClientAuthCertChild, override)

  explicit SelectTLSClientAuthCertChild(
      ClientAuthCertificateSelected* continuation);

  ipc::IPCResult RecvTLSClientAuthCertSelected(
      ByteArray&& aSelectedCertBytes,
      nsTArray<ByteArray>&& aSelectedCertChainBytes);

 private:
  ~SelectTLSClientAuthCertChild() = default;

  RefPtr<ClientAuthCertificateSelected> mContinuation;
};

}  
}  

#endif  // SECURITY_MANAGER_SSL_SELECTTLSCLIENTAUTHCERTCHILD_H_
