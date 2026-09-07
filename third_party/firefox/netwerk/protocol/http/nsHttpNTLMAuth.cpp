/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HttpLog.h"

#include "nsHttpNTLMAuth.h"
#include "nsIAuthModule.h"
#include "nsCOMPtr.h"
#include "nsServiceManagerUtils.h"
#include "plbase64.h"
#include "prnetdb.h"


#include "nsIHttpAuthenticableChannel.h"
#include "nsIURI.h"
#include "mozilla/Base64.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"
#include "mozilla/Tokenizer.h"
#include "nsCRT.h"
#include "nsNetUtil.h"
#include "nsIChannel.h"
#include "nsUnicharUtils.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/net/DNS.h"

namespace mozilla {
namespace net {

StaticRefPtr<nsHttpNTLMAuth> nsHttpNTLMAuth::gSingleton;


already_AddRefed<nsIHttpAuthenticator> nsHttpNTLMAuth::GetOrCreate() {
  nsCOMPtr<nsIHttpAuthenticator> authenticator;
  if (gSingleton) {
    authenticator = gSingleton;
  } else {
    gSingleton = new nsHttpNTLMAuth();
    ClearOnShutdown(&gSingleton);
    authenticator = gSingleton;
  }

  return authenticator.forget();
}

NS_IMPL_ISUPPORTS(nsHttpNTLMAuth, nsIHttpAuthenticator)

NS_IMETHODIMP
nsHttpNTLMAuth::ChallengeReceived(nsIHttpAuthenticableChannel* channel,
                                  const nsACString& challenge, bool isProxyAuth,
                                  nsISupports** sessionState,
                                  nsISupports** continuationState,
                                  bool* identityInvalid) {
  LOG(("nsHttpNTLMAuth::ChallengeReceived [ss=%p cs=%p]\n", *sessionState,
       *continuationState));

  mUseNative = true;


  *identityInvalid = false;

  if (challenge.Equals("NTLM"_ns, nsCaseInsensitiveCStringComparator)) {
    LOG(("No ntlm auth modules available.\n"));
    return NS_ERROR_UNEXPECTED;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsHttpNTLMAuth::GenerateCredentialsAsync(
    nsIHttpAuthenticableChannel* authChannel,
    nsIHttpAuthenticatorCallback* aCallback, const nsACString& challenge,
    bool isProxyAuth, const nsAString& domain, const nsAString& username,
    const nsAString& password, nsISupports* sessionState,
    nsISupports* continuationState, nsICancelable** aCancellable) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsHttpNTLMAuth::GenerateCredentials(
    nsIHttpAuthenticableChannel* authChannel, const nsACString& aChallenge,
    bool isProxyAuth, const nsAString& domain, const nsAString& user,
    const nsAString& pass, nsISupports** sessionState,
    nsISupports** continuationState, uint32_t* aFlags, nsACString& creds)

{
  LOG(("nsHttpNTLMAuth::GenerateCredentials\n"));

  creds.Truncate();
  *aFlags = 0;

  if (user.IsEmpty() || pass.IsEmpty()) *aFlags = USING_INTERNAL_IDENTITY;

  nsresult rv;
  nsCOMPtr<nsIAuthModule> module = do_QueryInterface(*continuationState, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  void *inBuf, *outBuf;
  uint32_t inBufLen, outBufLen;
  Maybe<nsTArray<uint8_t>> certArray;

  if (aChallenge.Equals("NTLM"_ns, nsCaseInsensitiveCStringComparator)) {
    nsCOMPtr<nsIURI> uri;
    rv = authChannel->GetURI(getter_AddRefs(uri));
    if (NS_FAILED(rv)) return rv;
    nsAutoCString serviceName, host;
    rv = uri->GetAsciiHost(host);
    if (NS_FAILED(rv)) return rv;
    serviceName.AppendLiteral("HTTP@");
    serviceName.Append(host);
    uint32_t reqFlags = nsIAuthModule::REQ_DEFAULT;
    if (isProxyAuth) reqFlags |= nsIAuthModule::REQ_PROXY_AUTH;

    rv = module->Init(serviceName, reqFlags, domain, user, pass);
    if (NS_FAILED(rv)) return rv;

    inBufLen = 0;
    inBuf = nullptr;
#if 0 /* || defined (LINUX) */
    nsCOMPtr<nsIChannel> channel = do_QueryInterface(authChannel, &rv);
    if (NS_FAILED(rv)) return rv;

    nsCOMPtr<nsITransportSecurityInfo> securityInfo;
    rv = channel->GetSecurityInfo(getter_AddRefs(securityInfo));
    if (NS_FAILED(rv)) return rv;

    if (mUseNative && securityInfo) {
      nsCOMPtr<nsIX509Cert> cert;
      rv = securityInfo->GetServerCert(getter_AddRefs(cert));
      if (NS_FAILED(rv)) return rv;

      if (cert) {
        certArray.emplace();
        rv = cert->GetRawDER(*certArray);
        if (NS_FAILED(rv)) {
          return rv;
        }

        inBufLen = certArray->Length();
        inBuf = certArray->Elements();
      }
    }
#endif
  } else {
    if (aChallenge.Length() < 6) {
      return NS_ERROR_UNEXPECTED;  
    }

    nsDependentCSubstring challenge(aChallenge, 5);
    uint32_t len = challenge.Length();
    while (len > 0 && challenge[len - 1] == '=') {
      len--;
    }

    rv = Base64Decode(challenge.BeginReading(), len, (char**)&inBuf, &inBufLen);
    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  rv = module->GetNextToken(inBuf, inBufLen, &outBuf, &outBufLen);
  if (NS_SUCCEEDED(rv)) {
    CheckedUint32 credsLen = ((CheckedUint32(outBufLen) + 2) / 3) * 4;
    credsLen += 5;  
    credsLen += 1;  

    if (!credsLen.isValid()) {
      rv = NS_ERROR_FAILURE;
    } else {
      nsAutoCString encoded;
      (void)Base64Encode(nsDependentCSubstring((char*)outBuf, outBufLen),
                         encoded);
      creds = nsPrintfCString("NTLM %s", encoded.get());
    }

    free(outBuf);
  }

  if (inBuf && !certArray) {
    free(inBuf);
  }

  return rv;
}

NS_IMETHODIMP
nsHttpNTLMAuth::GetAuthFlags(uint32_t* flags) {
  *flags = CONNECTION_BASED | IDENTITY_INCLUDES_DOMAIN | IDENTITY_ENCRYPTED;
  return NS_OK;
}

}  
}  
