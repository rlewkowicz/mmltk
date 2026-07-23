/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AppTrustDomain.h"

#include "mozpkix/pkixnss.h"

using namespace mozilla::pkix;

namespace mozilla {
namespace psm {

AppTrustDomain::AppTrustDomain(nsTArray<Span<const uint8_t>>&& collectedCerts) {
  MOZ_ASSERT(collectedCerts.IsEmpty());
}

pkix::Result AppTrustDomain::FindIssuer(Input, IssuerChecker&, Time) {
  return pkix::Result::FATAL_ERROR_INVALID_STATE;
}

pkix::Result AppTrustDomain::GetCertTrust(EndEntityOrCA,
                                          const CertPolicyId& policy,
                                          Input,
                                           TrustLevel& trustLevel) {
  MOZ_ASSERT(policy.IsAnyPolicy());
  if (!policy.IsAnyPolicy()) {
    return pkix::Result::FATAL_ERROR_INVALID_ARGS;
  }

  trustLevel = TrustLevel::InheritsTrust;
  return Success;
}

pkix::Result AppTrustDomain::DigestBuf(Input item, DigestAlgorithm digestAlg,
                                        uint8_t* digestBuf,
                                       size_t digestBufLen) {
  return DigestBufNSS(item, digestAlg, digestBuf, digestBufLen);
}

pkix::Result AppTrustDomain::CheckRevocation(EndEntityOrCA, const CertID&, Time,
                                             Duration,
                                              const Input*,
                                              const Input*) {
  return Success;
}

pkix::Result AppTrustDomain::IsChainValid(const DERArray&, Time,
                                          const CertPolicyId& requiredPolicy) {
  MOZ_ASSERT(requiredPolicy.IsAnyPolicy());
  return Success;
}

pkix::Result AppTrustDomain::CheckSignatureDigestAlgorithm(
    DigestAlgorithm digestAlg, EndEntityOrCA, Time) {
  switch (digestAlg) {
    case DigestAlgorithm::sha256:  // fall through
    case DigestAlgorithm::sha384:  // fall through
    case DigestAlgorithm::sha512:
      return Success;
    case DigestAlgorithm::sha1:
      return pkix::Result::ERROR_CERT_SIGNATURE_ALGORITHM_DISABLED;
  }
  return pkix::Result::FATAL_ERROR_LIBRARY_FAILURE;
}

pkix::Result AppTrustDomain::CheckRSAPublicKeyModulusSizeInBits(
    EndEntityOrCA , unsigned int modulusSizeInBits) {
  if (modulusSizeInBits < 2048u) {
    return pkix::Result::ERROR_INADEQUATE_KEY_SIZE;
  }
  return Success;
}

pkix::Result AppTrustDomain::VerifyRSAPKCS1SignedData(
    Input data, DigestAlgorithm digestAlgorithm, Input signature,
    Input subjectPublicKeyInfo) {
  return VerifyRSAPKCS1SignedDataNSS(data, digestAlgorithm, signature,
                                     subjectPublicKeyInfo, nullptr);
}

pkix::Result AppTrustDomain::VerifyRSAPSSSignedData(
    Input data, DigestAlgorithm digestAlgorithm, Input signature,
    Input subjectPublicKeyInfo) {
  return VerifyRSAPSSSignedDataNSS(data, digestAlgorithm, signature,
                                   subjectPublicKeyInfo, nullptr);
}

pkix::Result AppTrustDomain::CheckECDSACurveIsAcceptable(
    EndEntityOrCA , NamedCurve curve) {
  switch (curve) {
    case NamedCurve::secp256r1:  // fall through
    case NamedCurve::secp384r1:  // fall through
    case NamedCurve::secp521r1:
      return Success;
  }

  return pkix::Result::ERROR_UNSUPPORTED_ELLIPTIC_CURVE;
}

pkix::Result AppTrustDomain::VerifyECDSASignedData(
    Input data, DigestAlgorithm digestAlgorithm, Input signature,
    Input subjectPublicKeyInfo) {
  return VerifyECDSASignedDataNSS(data, digestAlgorithm, signature,
                                  subjectPublicKeyInfo, nullptr);
}

pkix::Result AppTrustDomain::CheckValidityIsAcceptable(
    Time , Time , EndEntityOrCA ,
    KeyPurposeId ) {
  return Success;
}

void AppTrustDomain::NoteAuxiliaryExtension(AuxiliaryExtension ,
                                            Input ) {}

}  
}  
