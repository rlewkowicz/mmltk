/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef AppSignatureVerification_h
#define AppSignatureVerification_h

#include "mozpkix/pkix.h"
#include "mozpkix/pkixnss.h"
#include "mozpkix/pkixutil.h"

mozilla::Span<const uint8_t> GetPKCS7SignerCert(
    NSSCMSSignerInfo* signerInfo,
    nsTArray<mozilla::Span<const uint8_t>>& collectedCerts);

NSSCMSSignedData* GetSignedDataContent(NSSCMSMessage* cmsg);

void CollectCertificates(
    NSSCMSSignedData* signedData,
     nsTArray<mozilla::Span<const uint8_t>>& collectedCerts);

nsresult VerifySignatureFromCertificate(
    mozilla::Span<const uint8_t> signerCertSpan, NSSCMSSignerInfo* signerInfo,
    SECItem* detachedDigest);

void GetAllSignerInfosForSupportedDigestAlgorithms(
    NSSCMSSignedData* signedData,
     nsTArray<std::tuple<NSSCMSSignerInfo*, SECOidTag>>& signerInfos);
#endif  // AppSignatureVerification_h
