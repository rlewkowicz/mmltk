/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include <stdlib.h>
#include <stdio.h>
#include "cryptox.h"

#if defined(MAR_NSS)

CryptoX_Result NSS_LoadPublicKey(const unsigned char* certData,
                                 unsigned int certDataSize,
                                 SECKEYPublicKey** publicKey) {
  CERTCertificate* cert;
  SECItem certDataItem = {siBuffer, (unsigned char*)certData, certDataSize};

  if (!certData || !publicKey) {
    return CryptoX_Error;
  }

  cert = CERT_NewTempCertificate(CERT_GetDefaultCertDB(), &certDataItem, NULL,
                                 PR_FALSE, PR_TRUE);
  if (!cert) {
    return CryptoX_Error;
  }
  *publicKey = CERT_ExtractPublicKey(cert);
  CERT_DestroyCertificate(cert);

  if (!*publicKey) {
    return CryptoX_Error;
  }
  return CryptoX_Success;
}

CryptoX_Result NSS_VerifyBegin(VFYContext** ctx,
                               SECKEYPublicKey* const* publicKey) {
  SECStatus status;
  if (!ctx || !publicKey || !*publicKey) {
    return CryptoX_Error;
  }

  if ((SECKEY_PublicKeyStrength(*publicKey) * 8) <
      XP_MIN_SIGNATURE_LEN_IN_BYTES) {
    fprintf(stderr, "ERROR: Key length must be >= %d bytes\n",
            XP_MIN_SIGNATURE_LEN_IN_BYTES);
    return CryptoX_Error;
  }

  *ctx = VFY_CreateContext(*publicKey, NULL,
                           SEC_OID_PKCS1_SHA384_WITH_RSA_ENCRYPTION, NULL);
  if (*ctx == NULL) {
    return CryptoX_Error;
  }

  status = VFY_Begin(*ctx);
  return SECSuccess == status ? CryptoX_Success : CryptoX_Error;
}

CryptoX_Result NSS_VerifySignature(VFYContext* const* ctx,
                                   const unsigned char* signature,
                                   unsigned int signatureLen) {
  SECItem signedItem;
  SECStatus status;
  if (!ctx || !signature || !*ctx) {
    return CryptoX_Error;
  }

  signedItem.len = signatureLen;
  signedItem.data = (unsigned char*)signature;
  status = VFY_EndWithSignature(*ctx, &signedItem);
  return SECSuccess == status ? CryptoX_Success : CryptoX_Error;
}

#endif
