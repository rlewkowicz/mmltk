/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(CRYPTOX_H)
#define CRYPTOX_H

#define XP_MIN_SIGNATURE_LEN_IN_BYTES 256

#define CryptoX_Result int
#define CryptoX_Success 0
#define CryptoX_Error (-1)

#if defined(MAR_NSS)

#  include "cert.h"
#  include "keyhi.h"
#  include "cryptohi.h"

#  define CryptoX_InvalidHandleValue NULL
#  define CryptoX_ProviderHandle void*
#  define CryptoX_SignatureHandle VFYContext*
#  define CryptoX_PublicKey SECKEYPublicKey*
#  define CryptoX_Certificate CERTCertificate*

#if defined(__cplusplus)
extern "C" {
#endif
CryptoX_Result NSS_LoadPublicKey(const unsigned char* certData,
                                 unsigned int certDataSize,
                                 SECKEYPublicKey** publicKey);
CryptoX_Result NSS_VerifyBegin(VFYContext** ctx,
                               SECKEYPublicKey* const* publicKey);
CryptoX_Result NSS_VerifySignature(VFYContext* const* ctx,
                                   const unsigned char* signature,
                                   unsigned int signatureLen);
#if defined(__cplusplus)
}  
#endif

#  define CryptoX_InitCryptoProvider(CryptoHandle) CryptoX_Success
#  define CryptoX_VerifyBegin(CryptoHandle, SignatureHandle, PublicKey) \
    NSS_VerifyBegin(SignatureHandle, PublicKey)
#  define CryptoX_FreeSignatureHandle(SignatureHandle) \
    VFY_DestroyContext(*SignatureHandle, PR_TRUE)
#  define CryptoX_VerifyUpdate(SignatureHandle, buf, len) \
    VFY_Update(*SignatureHandle, (const unsigned char*)(buf), len)
#  define CryptoX_LoadPublicKey(CryptoHandle, certData, dataSize, publicKey) \
    NSS_LoadPublicKey(certData, dataSize, publicKey)
#  define CryptoX_VerifySignature(hash, publicKey, signedData, len) \
    NSS_VerifySignature(hash, (const unsigned char*)(signedData), len)
#  define CryptoX_FreePublicKey(key) SECKEY_DestroyPublicKey(*key)
#  define CryptoX_FreeCertificate(cert) CERT_DestroyCertificate(*cert)

#else


#  define CryptoX_InvalidHandleValue NULL
#  define CryptoX_ProviderHandle void*
#  define CryptoX_SignatureHandle void*
#  define CryptoX_PublicKey void*
#  define CryptoX_Certificate void*
#  define CryptoX_InitCryptoProvider(CryptoHandle) CryptoX_Error
#  define CryptoX_VerifyBegin(CryptoHandle, SignatureHandle, PublicKey) \
    CryptoX_Error
#  define CryptoX_FreeSignatureHandle(SignatureHandle)
#  define CryptoX_VerifyUpdate(SignatureHandle, buf, len) CryptoX_Error
#  define CryptoX_LoadPublicKey(CryptoHandle, certData, dataSize, publicKey) \
    CryptoX_Error
#  define CryptoX_VerifySignature(hash, publicKey, signedData, len) \
    CryptoX_Error
#  define CryptoX_FreePublicKey(key) CryptoX_Error

#endif

#endif
