/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SECURITY_MANAGER_SSL_SELECTTLSCLIENTAUTHCERTPARENT_H_
#define SECURITY_MANAGER_SSL_SELECTTLSCLIENTAUTHCERTPARENT_H_

#include "mozilla/OriginAttributes.h"
#include "mozilla/psm/PSelectTLSClientAuthCertParent.h"

namespace mozilla {
namespace psm {

class SelectTLSClientAuthCertParent : public PSelectTLSClientAuthCertParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(SelectTLSClientAuthCertParent, override)

  SelectTLSClientAuthCertParent() = default;

  bool Dispatch(const nsACString& aHostName,
                const OriginAttributes& aOriginAttributes, const int32_t& aPort,
                const uint32_t& aProviderFlags,
                const uint32_t& aProviderTlsFlags,
                const ByteArray& aServerCertBytes,
                nsTArray<ByteArray>&& aCANames,
                const uint64_t& aBrowsingContextID);

  void TLSClientAuthCertSelected(
      const nsTArray<uint8_t>& aSelectedCertBytes,
      nsTArray<nsTArray<uint8_t>>&& aSelectedCertChainBytes);

 private:
  ~SelectTLSClientAuthCertParent() = default;

  void ActorDestroy(mozilla::ipc::IProtocol::ActorDestroyReason aWhy) override;
};

}  
}  

#endif  // SECURITY_MANAGER_SSL_SELECTTLSCLIENTAUTHCERTPARENT_H_
