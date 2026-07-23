/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
/* Copyright 2013 Mozilla Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef mozilla_pkix_pkix_h
#define mozilla_pkix_pkix_h

#include "mozpkix/pkixtypes.h"

namespace mozilla {
namespace pkix {


Result BuildCertChain(TrustDomain& trustDomain, Input cert, Time time,
                      EndEntityOrCA endEntityOrCA,
                      KeyUsage requiredKeyUsageIfPresent,
                      KeyPurposeId requiredEKUIfPresent,
                      const CertPolicyId& requiredPolicy,
                       const Input* stapledOCSPResponse);

Result CheckCertHostname(Input cert, Input hostname);
Result CheckCertHostname(Input cert, Input hostname,
                         NameMatchingPolicy& nameMatchingPolicy);

static const size_t OCSP_REQUEST_MAX_LENGTH = 127;
Result CreateEncodedOCSPRequest(TrustDomain& trustDomain, const CertID& certID,
                                 uint8_t (&out)[OCSP_REQUEST_MAX_LENGTH],
                                 size_t& outLen);

Result VerifyEncodedOCSPResponse(
    TrustDomain& trustDomain, const CertID& certID, Time time,
    uint16_t maxLifetimeInDays, Input encodedResponse,
     bool& expired,
     Time* thisUpdate = nullptr,
     Time* validThrough = nullptr);

Result CheckTLSFeaturesAreSatisfied(Input& cert,
                                    const Input* stapledOCSPResponse);
}  
}  

#endif  // mozilla_pkix_pkix_h
