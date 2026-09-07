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

#ifndef mozilla_pkix_pkixcheck_h
#define mozilla_pkix_pkixcheck_h

#include "mozpkix/pkixtypes.h"

namespace mozilla {
namespace pkix {

class BackCert;

Result CheckIssuerIndependentProperties(TrustDomain& trustDomain,
                                        const BackCert& cert, Time time,
                                        KeyUsage requiredKeyUsageIfPresent,
                                        KeyPurposeId requiredEKUIfPresent,
                                        const CertPolicyId& requiredPolicy,
                                        unsigned int subCACount,
                                         TrustLevel& trustLevel);

Result CheckNameConstraints(Input encodedNameConstraints,
                            const BackCert& firstChild,
                            KeyPurposeId requiredEKUIfPresent);

Result CheckIssuer(Input encodedIssuer);

Result ParseValidity(Input encodedValidity,
                      Time* notBeforeOut = nullptr,
                      Time* notAfterOut = nullptr);
Result CheckValidity(Time time, Time notBefore, Time notAfter);

Result CheckTLSFeatures(const BackCert& subject, BackCert& potentialIssuer);
}  
}  

#endif  // mozilla_pkix_pkixcheck_h
