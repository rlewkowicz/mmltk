/*
 * This file is PRIVATE to SSL.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __tls13psk_h_
#define __tls13psk_h_


struct sslPskStr {
    PRCList link;
    PK11SymKey *key;              
    PK11SymKey *binderKey;        
    SSLPskType type;              
    SECItem label;                
    SSLHashType hash;             
    ssl3CipherSuite zeroRttSuite; 
    PRUint32 maxEarlyData;        
};

SECStatus SSLExp_AddExternalPsk(PRFileDesc *fd, PK11SymKey *psk, const PRUint8 *identity,
                                unsigned int identitylen, SSLHashType hash);

SECStatus SSLExp_AddExternalPsk0Rtt(PRFileDesc *fd, PK11SymKey *psk, const PRUint8 *identity,
                                    unsigned int identitylen, SSLHashType hash,
                                    PRUint16 zeroRttSuite, PRUint32 maxEarlyData);

SECStatus SSLExp_RemoveExternalPsk(PRFileDesc *fd, const PRUint8 *identity, unsigned int identitylen);

sslPsk *tls13_CopyPsk(sslPsk *opsk);

void tls13_DestroyPsk(sslPsk *psk);

void tls13_DestroyPskList(PRCList *list);

sslPsk *tls13_MakePsk(PK11SymKey *key, SSLPskType pskType, SSLHashType hashType, const SECItem *label);

SECStatus tls13_ResetHandshakePsks(sslSocket *ss, PRCList *list);

#endif
