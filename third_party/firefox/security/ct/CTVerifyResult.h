/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CTVerifyResult_h
#define CTVerifyResult_h

#include <vector>

#include "CTKnownLogs.h"
#include "CTLog.h"
#include "SignedCertificateTimestamp.h"

namespace mozilla {
namespace ct {

enum class SCTOrigin {
  Embedded,
  TLSExtension,
  OCSPResponse,
};

struct VerifiedSCT {
  VerifiedSCT(SignedCertificateTimestamp&& sct, SCTOrigin origin,
              CTLogOperatorId logOperatorId, CTLogState logState,
              CTLogFormat logFormat, uint64_t logTimestamp);

  SignedCertificateTimestamp sct;
  SCTOrigin origin;
  CTLogOperatorId logOperatorId;
  CTLogState logState;
  CTLogFormat logFormat;
  uint64_t logTimestamp;
};

typedef std::vector<VerifiedSCT> VerifiedSCTList;

class CTVerifyResult {
 public:
  CTVerifyResult() { Reset(); }

  VerifiedSCTList verifiedScts;

  size_t decodingErrors;
  size_t sctsFromUnknownLogs;
  size_t sctsWithInvalidSignatures;
  size_t sctsWithInvalidTimestamps;
  size_t sctsWithDistrustedTimestamps;

  size_t embeddedSCTs;
  size_t sctsFromTLSHandshake;
  size_t sctsFromOCSP;

  void Reset();
};

}  
}  

#endif  // CTVerifyResult_h
