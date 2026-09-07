/*
 * This file is PRIVATE to SSL.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __sslcert_h_
#define __sslcert_h_

#include "cert.h"
#include "secitem.h"
#include "keyhi.h"

typedef PRUint16 sslAuthTypeMask;
PR_STATIC_ASSERT(sizeof(sslAuthTypeMask) * 8 >= ssl_auth_size);

typedef struct sslServerCertStr {
    PRCList link; 

    sslAuthTypeMask authTypes;
    const sslNamedGroupDef *namedCurve;

    CERTCertificate *serverCert;
    CERTCertificateList *serverCertChain;
    sslKeyPair *serverKeyPair;
    unsigned int serverKeyBits;
    SECItemArray *certStatusArray;
    SECItem signedCertTimestamps;

    SECItem delegCred;
    sslKeyPair *delegCredKeyPair;
} sslServerCert;

#define SSL_CERT_IS(c, t) ((c)->authTypes & (1 << (t)))
#define SSL_CERT_IS_ONLY(c, t) ((c)->authTypes == (1 << (t)))
#define SSL_CERT_IS_EC(c)                         \
    ((c)->authTypes & ((1 << ssl_auth_ecdsa) |    \
                       (1 << ssl_auth_ecdh_rsa) | \
                       (1 << ssl_auth_ecdh_ecdsa)))

extern sslServerCert *ssl_NewServerCert();
extern sslServerCert *ssl_CopyServerCert(const sslServerCert *oc);
extern const sslServerCert *ssl_FindServerCert(
    const sslSocket *ss, SSLAuthType authType,
    const sslNamedGroupDef *namedCurve);
extern void ssl_FreeServerCert(sslServerCert *sc);

#endif /* __sslcert_h_ */
