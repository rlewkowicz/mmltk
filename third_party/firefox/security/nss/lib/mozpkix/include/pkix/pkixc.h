/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef mozilla_pkix_pkixc_h
#define mozilla_pkix_pkixc_h

#include "prerror.h"
#include "stdint.h"


#ifdef __cplusplus
extern "C" {
#endif
bool VerifyCodeSigningCertificateChain(const uint8_t** certificates,
                                       const uint16_t* certificateLengths,
                                       size_t numCertificates,
                                       uint64_t secondsSinceEpoch,
                                       const uint8_t* rootSHA256Hash,
                                       const uint8_t* hostname,
                                       size_t hostnameLength,
                                        PRErrorCode* error);
#ifdef __cplusplus
}
#endif

#endif  // mozilla_pkix_pkixc_h
