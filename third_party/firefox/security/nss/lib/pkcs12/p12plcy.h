/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _P12PLCY_H_
#define _P12PLCY_H_

#include "secoid.h"
#include "ciferfam.h"

SEC_BEGIN_PROTOS

extern PRBool SEC_PKCS12CipherAllowed(SECOidTag pbeAlg, SECOidTag hmacAlg);

extern PRBool SEC_PKCS12DecryptionAllowed(SECAlgorithmID *algid);

extern PRBool SEC_PKCS12IntegrityHashAllowed(SECOidTag hashAlg, PRBool verify);

extern PRBool SEC_PKCS12IsEncryptionAllowed(void);

extern SECStatus SEC_PKCS12EnableCipher(long which, int on);

extern SECStatus SEC_PKCS12SetPreferredCipher(long which, int on);

SEC_END_PROTOS
#endif
