/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef XiphExtradata_h
#define XiphExtradata_h

#include "MediaData.h"

namespace mozilla {

bool XiphHeadersToExtradata(MediaByteBuffer* aCodecSpecificConfig,
                            const nsTArray<const unsigned char*>& aHeaders,
                            const nsTArray<size_t>& aHeaderLens);

bool XiphExtradataToHeaders(nsTArray<unsigned char*>& aHeaders,
                            nsTArray<size_t>& aHeaderLens, unsigned char* aData,
                            size_t aAvailable);

}  

#endif  // XiphExtradata_h
