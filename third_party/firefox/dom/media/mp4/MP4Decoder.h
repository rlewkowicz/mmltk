/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(MP4Decoder_h_)
#  define MP4Decoder_h_

#  include "mozilla/UniquePtr.h"
#  include "nsStringFwd.h"
#  include "nsTArray.h"

namespace mozilla {

class MediaContainerType;
class MediaResult;
class DecoderDoctorDiagnostics;
class TrackInfo;

class MP4Decoder {
 public:
  static bool IsSupportedType(const MediaContainerType& aContainerType,
                              DecoderDoctorDiagnostics* aDiagnostics);

  static bool IsH264(const nsACString& aMimeType);

  static bool IsAAC(const nsACString& aMimeType);

  static bool IsHEVC(const nsACString& aMimeType);

  static bool IsEnabled();

  static nsTArray<UniquePtr<TrackInfo>> GetTracksInfo(
      const MediaContainerType& aType);

 private:
  static nsTArray<UniquePtr<TrackInfo>> GetTracksInfo(
      const MediaContainerType& aType, MediaResult& aError);
};

}  

#endif
