/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_dom_WebAuthnAutoFillEntry_h_)
#define mozilla_dom_WebAuthnAutoFillEntry_h_

#include "nsIWebAuthnService.h"
#include "nsString.h"


namespace mozilla::dom {

class WebAuthnAutoFillEntry final : public nsIWebAuthnAutoFillEntry {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIWEBAUTHNAUTOFILLENTRY

  WebAuthnAutoFillEntry(uint8_t aProvider, const nsAString& aUserName,
                        const nsAString& aRpId,
                        const nsTArray<uint8_t>& aCredentialId)
      : mProvider(aProvider), mUserName(aUserName), mRpId(aRpId) {
    mCredentialId.Assign(aCredentialId);
  }


 private:
  ~WebAuthnAutoFillEntry() = default;

  uint8_t mProvider;
  nsString mUserName;
  nsString mRpId;
  nsTArray<uint8_t> mCredentialId;
};

}  
#endif
