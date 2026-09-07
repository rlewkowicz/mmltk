/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BTVerifier_h
#define BTVerifier_h

#include "BTTypes.h"
#include "mozpkix/Input.h"
#include "mozpkix/Result.h"
#include "mozpkix/pkixder.h"
#include "mozpkix/pkixtypes.h"

namespace mozilla {
namespace ct {

pkix::Result DecodeAndVerifySignedTreeHead(
    pkix::Input signerSubjectPublicKeyInfo,
    pkix::DigestAlgorithm digestAlgorithm,
    pkix::der::PublicKeyAlgorithm publicKeyAlg, pkix::Input signedTreeHeadInput,
    SignedTreeHeadDataV2& signedTreeHead);

pkix::Result DecodeInclusionProof(pkix::Input input,
                                  InclusionProofDataV2& output);
pkix::Result VerifyInclusionProof(const InclusionProofDataV2& proof,
                                  pkix::Input leafEntry,
                                  pkix::Input expectedRootHash,
                                  pkix::DigestAlgorithm digestAlgorithm);

}  
}  

#endif  // BTVerifier_h
