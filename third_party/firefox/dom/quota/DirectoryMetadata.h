/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_DIRECTORYMETADATA_H_
#define DOM_QUOTA_DIRECTORYMETADATA_H_

#include <cstdint>

enum class nsresult : uint32_t;

class nsIBinaryInputStream;
class nsIBinaryOutputStream;
class nsIFile;

namespace mozilla {

template <typename V, typename E>
class Result;

}

namespace mozilla::dom::quota {

struct OriginStateMetadata;


Result<OriginStateMetadata, nsresult> ReadDirectoryMetadataHeader(
    nsIBinaryInputStream& aStream);

nsresult WriteDirectoryMetadataHeader(
    nsIBinaryOutputStream& aStream,
    const OriginStateMetadata& aOriginStateMetadata);

Result<OriginStateMetadata, nsresult> LoadDirectoryMetadataHeader(
    nsIFile& aDirectory);

nsresult SaveDirectoryMetadataHeader(
    nsIFile& aDirectory, const OriginStateMetadata& aOriginStateMetadata);

}  

#endif  // DOM_QUOTA_NOTIFYUTILS_H_
