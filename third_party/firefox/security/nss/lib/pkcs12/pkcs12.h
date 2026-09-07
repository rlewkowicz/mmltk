/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _PKCS12_H_
#define _PKCS12_H_

#include "pkcs12t.h"
#include "p12.h"

SEC_BEGIN_PROTOS

typedef SECItem *(*SEC_PKCS12GetPassword)(void *arg);

SECStatus
SEC_PKCS12PutPFX(SECItem *der_pfx, SECItem *pwitem,
                 SEC_PKCS12NicknameCollisionCallback ncCall,
                 PK11SlotInfo *slot, void *wincx);

PRBool SEC_PKCS12ValidData(char *buf, int bufLen, long int totalLength);

SEC_END_PROTOS

#endif
