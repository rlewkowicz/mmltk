/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ExtendedValidation_h
#define ExtendedValidation_h

#include "ScopedNSSTypes.h"
#include "certt.h"

namespace mozilla {
namespace pkix {
struct CertPolicyId;
}  
}  

namespace mozilla {
namespace psm {

nsresult LoadExtendedValidationInfo();

void GetKnownEVPolicies(
    const nsTArray<uint8_t>& cert,
     nsTArray<mozilla::pkix::CertPolicyId>& policies);

bool CertIsAuthoritativeForEVPolicy(const nsTArray<uint8_t>& cert,
                                    const mozilla::pkix::CertPolicyId& policy);

}  
}  

#endif  // ExtendedValidation_h
