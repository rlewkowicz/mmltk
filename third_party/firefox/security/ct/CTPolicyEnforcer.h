/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CTPolicyEnforcer_h
#define CTPolicyEnforcer_h

#include "CTLog.h"
#include "CTVerifyResult.h"
#include "mozpkix/Result.h"
#include "mozpkix/Time.h"

namespace mozilla {
namespace ct {

enum class CTPolicyCompliance {
  Compliant,
  NotEnoughScts,
  NotDiverseScts,
};

CTPolicyCompliance CheckCTPolicyCompliance(const VerifiedSCTList& verifiedScts,
                                           pkix::Duration certLifetime);

}  
}  

#endif  // CTPolicyEnforcer_h
