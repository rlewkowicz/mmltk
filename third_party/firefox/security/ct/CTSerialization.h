/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CTSerialization_h
#define CTSerialization_h

#include <vector>

#include "mozpkix/Input.h"
#include "mozpkix/Result.h"
#include "SignedCertificateTimestamp.h"

namespace mozilla {
namespace ct {

pkix::Result EncodeDigitallySigned(const DigitallySigned& data, Buffer& output);

pkix::Result DecodeDigitallySigned(pkix::Reader& reader,
                                   DigitallySigned& output);

pkix::Result EncodeLogEntry(const LogEntry& entry, Buffer& output);

pkix::Result EncodeV1SCTSignedData(uint64_t timestamp,
                                   pkix::Input serializedLogEntry,
                                   pkix::Input extensions, Buffer& output);

pkix::Result DecodeSCTList(pkix::Input input, pkix::Reader& listReader);

pkix::Result ReadSCTListItem(pkix::Reader& listReader, pkix::Input& result);

pkix::Result DecodeSignedCertificateTimestamp(
    pkix::Reader& input, SignedCertificateTimestamp& output);

pkix::Result EncodeSCTList(const std::vector<pkix::Input>& scts,
                           Buffer& output);

}  
}  

#endif  // CTSerialization_h
