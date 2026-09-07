/*
 * This file is PRIVATE to SSL.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __tls13subcerts_h_
#define __tls13subcerts_h_

struct sslDelegatedCredentialStr {
    PRUint32 validTime;

    SSLSignatureScheme expectedCertVerifyAlg;

    SECItem derSpki;

    CERTSubjectPublicKeyInfo *spki;

    SSLSignatureScheme alg;

    SECItem signature;
};

SECStatus tls13_ReadDelegatedCredential(PRUint8 *b,
                                        PRUint32 length,
                                        sslDelegatedCredential **dcp);
void tls13_DestroyDelegatedCredential(sslDelegatedCredential *dc);

PRBool tls13_IsVerifyingWithDelegatedCredential(const sslSocket *ss);
PRBool tls13_IsSigningWithDelegatedCredential(const sslSocket *ss);
SECStatus tls13_MaybeSetDelegatedCredential(sslSocket *ss);
SECStatus tls13_VerifyDelegatedCredential(sslSocket *ss,
                                          sslDelegatedCredential *dc);

SECStatus SSLExp_DelegateCredential(const CERTCertificate *cert,
                                    const SECKEYPrivateKey *certPriv,
                                    const SECKEYPublicKey *dcPub,
                                    SSLSignatureScheme dcCertVerifyAlg,
                                    PRUint32 dcValidFor,
                                    PRTime now,
                                    SECItem *out);

#endif
