/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_dom_WebAuthnService_h_)
#define mozilla_dom_WebAuthnService_h_

#include "AuthrsBridge_ffi.h"
#include "WebAuthnArgs.h"
#include "mozilla/StaticPrefs_security.h"
#include "mozilla/dom/WebAuthnPromiseHolder.h"
#include "nsIWebAuthnService.h"




namespace mozilla::dom {

already_AddRefed<nsIWebAuthnService> NewWebAuthnService();

class WebAuthnService final : public nsIWebAuthnService {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIWEBAUTHNSERVICE

  WebAuthnService() {
    (void)authrs_service_constructor(getter_AddRefs(mAuthrsService));
    mPlatformService = mAuthrsService;
  }

 private:
  ~WebAuthnService() = default;

  struct TransactionState {
    nsCOMPtr<nsIWebAuthnService> service;
    uint64_t transactionId;
    Maybe<nsCOMPtr<nsIWebAuthnRegisterPromise>> parentRegisterPromise;
    Maybe<nsCOMPtr<nsIWebAuthnRegisterResult>> registerResult;
    MozPromiseRequestHolder<WebAuthnRegisterPromise> childRegisterRequest;
  };

  struct ConditionalGet {
    uint64_t transactionId;
    uint64_t browsingContextId;
    nsCOMPtr<nsIWebAuthnSignArgs> signArgs;
    nsCOMPtr<nsIWebAuthnSignPromise> signPromise;
  };

  Maybe<TransactionState> mActiveTransaction;
  nsTArray<ConditionalGet> mConditionalGets;

  void ShowAttestationConsentPrompt(const nsString& aOrigin,
                                    uint64_t aTransactionId,
                                    uint64_t aBrowsingContextId);
  void RejectActiveRegisterPromise();
  void ResetActiveTransaction();
  Maybe<ConditionalGet> TakeConditionalByTid(uint64_t aTransactionId);
  nsresult DispatchConditionalGetAssertion(const ConditionalGet& aPending,
                                           nsIWebAuthnSignArgs* aArgs);

  nsIWebAuthnService* DefaultService() {
    if (StaticPrefs::security_webauth_webauthn_enable_softtoken()) {
      return mAuthrsService;
    }
    return mPlatformService;
  }

  nsIWebAuthnService* AuthrsService() { return mAuthrsService; }

  nsIWebAuthnService* ActiveService() {
    MOZ_ASSERT(NS_IsMainThread());
    MOZ_ASSERT(mActiveTransaction.isSome());
    return mActiveTransaction.ref().service;
  }

  nsCOMPtr<nsIWebAuthnService> mAuthrsService;
  nsCOMPtr<nsIWebAuthnService> mPlatformService;
};

}  

#endif
