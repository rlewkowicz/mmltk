/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef _PKCS1SIG_H_
#define _PKCS1SIG_H_

#include "hasht.h"
#include "seccomon.h"
#include "secoidt.h"

SECStatus _SGN_VerifyPKCS1DigestInfo(SECOidTag digestAlg,
                                     const SECItem* digest,
                                     const SECItem* dataRecoveredFromSignature,
                                     PRBool unsafeAllowMissingParameters);

#endif /* _PKCS1SIG_H_ */
