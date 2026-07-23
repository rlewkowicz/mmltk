/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_dom_WebAuthnResult_h_)
#define mozilla_dom_WebAuthnResult_h_

#include "mozilla/Maybe.h"
#include "nsIWebAuthnResult.h"
#include "nsString.h"
#include "nsTArray.h"



namespace mozilla::dom {

class WebAuthnRegisterResult final : public nsIWebAuthnRegisterResult {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIWEBAUTHNREGISTERRESULT

  WebAuthnRegisterResult(const nsTArray<uint8_t>& aAttestationObject,
                         const Maybe<nsCString>& aClientDataJSON,
                         const nsTArray<uint8_t>& aCredentialId,
                         const nsTArray<nsString>& aTransports,
                         const Maybe<nsString>& aAuthenticatorAttachment,
                         const Maybe<bool>& aLargeBlobSupported,
                         const Maybe<bool>& aPrfSupported,
                         const Maybe<nsTArray<uint8_t>>& aPrfFirst,
                         const Maybe<nsTArray<uint8_t>>& aPrfSecond)
      : mAttestationConsentPromptShown(false),
        mClientDataJSON(aClientDataJSON),
        mCredPropsRk(Nothing()),
        mAuthenticatorAttachment(aAuthenticatorAttachment),
        mLargeBlobSupported(aLargeBlobSupported),
        mPrfSupported(aPrfSupported) {
    mAttestationObject.AppendElements(aAttestationObject);
    mCredentialId.AppendElements(aCredentialId);
    mTransports.AppendElements(aTransports);
    if (aPrfFirst.isSome()) {
      mPrfFirst.emplace(aPrfFirst->Length());
      mPrfFirst->Assign(aPrfFirst.ref());
    }
    if (aPrfSecond.isSome()) {
      mPrfSecond.emplace(aPrfSecond->Length());
      mPrfSecond->Assign(aPrfSecond.ref());
    }
  }



 private:
  ~WebAuthnRegisterResult() = default;

  nsTArray<uint8_t> mAttestationObject;
  bool mAttestationConsentPromptShown;
  nsTArray<uint8_t> mCredentialId;
  nsTArray<nsString> mTransports;
  Maybe<nsCString> mClientDataJSON;
  Maybe<bool> mCredPropsRk;
  Maybe<bool> mHmacCreateSecret;
  Maybe<nsString> mAuthenticatorAttachment;
  Maybe<bool> mLargeBlobSupported;
  Maybe<bool> mPrfSupported;
  Maybe<nsTArray<uint8_t>> mPrfFirst;
  Maybe<nsTArray<uint8_t>> mPrfSecond;
};

class WebAuthnSignResult final : public nsIWebAuthnSignResult {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIWEBAUTHNSIGNRESULT

  WebAuthnSignResult(const nsTArray<uint8_t>& aAuthenticatorData,
                     const Maybe<nsCString>& aClientDataJSON,
                     const nsTArray<uint8_t>& aCredentialId,
                     const nsTArray<uint8_t>& aSignature,
                     const nsTArray<uint8_t>& aUserHandle,
                     const Maybe<nsString>& aAuthenticatorAttachment,
                     const Maybe<bool>& aUsedAppId,
                     const Maybe<nsTArray<uint8_t>>& aLargeBlobValue,
                     const Maybe<bool>& aLargeBlobWritten,
                     const Maybe<nsTArray<uint8_t>>& aPrfFirst,
                     const Maybe<nsTArray<uint8_t>>& aPrfSecond)
      : mClientDataJSON(aClientDataJSON),
        mAuthenticatorAttachment(aAuthenticatorAttachment),
        mUsedAppId(aUsedAppId),
        mLargeBlobWritten(aLargeBlobWritten) {
    mAuthenticatorData.AppendElements(aAuthenticatorData);
    mCredentialId.AppendElements(aCredentialId);
    mSignature.AppendElements(aSignature);
    mUserHandle.AppendElements(aUserHandle);
    if (aLargeBlobValue.isSome()) {
      mLargeBlobValue.emplace(aLargeBlobValue->Length());
      mLargeBlobValue->Assign(aLargeBlobValue.ref());
    }
    if (aPrfFirst.isSome()) {
      mPrfFirst.emplace(aPrfFirst.ref().Length());
      mPrfFirst->Assign(aPrfFirst.ref());
    }
    if (aPrfSecond.isSome()) {
      mPrfSecond.emplace(aPrfSecond.ref().Length());
      mPrfSecond->Assign(aPrfSecond.ref());
    }
  }



 private:
  ~WebAuthnSignResult() = default;

  nsTArray<uint8_t> mAuthenticatorData;
  Maybe<nsCString> mClientDataJSON;
  nsTArray<uint8_t> mCredentialId;
  nsTArray<uint8_t> mSignature;
  nsTArray<uint8_t> mUserHandle;
  Maybe<nsString> mAuthenticatorAttachment;
  Maybe<bool> mUsedAppId;
  Maybe<nsTArray<uint8_t>> mLargeBlobValue;
  Maybe<bool> mLargeBlobWritten;
  Maybe<nsTArray<uint8_t>> mPrfFirst;
  Maybe<nsTArray<uint8_t>> mPrfSecond;
};

}  
#endif
