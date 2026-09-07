/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ADTS_DECODER_H_
#define ADTS_DECODER_H_

#include "mozilla/UniquePtr.h"
#include "nsTArray.h"

namespace mozilla {

class MediaContainerType;
class TrackInfo;

class ADTSDecoder {
 public:
  static bool IsEnabled();
  static bool IsSupportedType(const MediaContainerType& aContainerType);
  static nsTArray<UniquePtr<TrackInfo>> GetTracksInfo(
      const MediaContainerType& aType);
};

}  

#endif  // !ADTS_DECODER_H_
