/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _mozilla_dom_fetch_FetchStreamUtils_h
#define _mozilla_dom_fetch_FetchStreamUtils_h

#include <cstdint>

#include "mozilla/NotNull.h"
#include "mozilla/dom/FetchTypes.h"
#include "nsIInputStream.h"

namespace mozilla {

namespace ipc {
class PBackgroundParent;
}

namespace dom {

NotNull<nsCOMPtr<nsIInputStream>> ToInputStream(
    const ParentToParentStream& aStream);

NotNull<nsCOMPtr<nsIInputStream>> ToInputStream(
    const ParentToChildStream& aStream);

ParentToParentStream ToParentToParentStream(
    const NotNull<nsCOMPtr<nsIInputStream>>& aStream, int64_t aStreamSize);

ParentToChildStream ToParentToChildStream(
    const NotNull<nsCOMPtr<nsIInputStream>>& aStream, int64_t aStreamSize,
    bool aSerializeAsLazy = true);

ParentToChildStream ToParentToChildStream(const ParentToParentStream& aStream,
                                          int64_t aStreamSize);

}  

}  

#endif  // _mozilla_dom_fetch_FetchStreamUtils_h
