/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CTObjectsExtractor_h
#define CTObjectsExtractor_h

#include "mozpkix/Input.h"
#include "mozpkix/Result.h"
#include "SignedCertificateTimestamp.h"

namespace mozilla {
namespace ct {

pkix::Result GetPrecertLogEntry(pkix::Input leafCertificate,
                                pkix::Input issuerSubjectPublicKeyInfo,
                                LogEntry& output);

void GetX509LogEntry(pkix::Input leafCertificate, LogEntry& output);

}  
}  

#endif  // CTObjectsExtractor_h
