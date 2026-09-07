/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef DOM_MEDIA_WEBM_MATROSKADEMUXER_H_
#define DOM_MEDIA_WEBM_MATROSKADEMUXER_H_

#include "WebMDemuxer.h"

namespace mozilla {

class MatroskaDemuxer : public WebMDemuxer {
 public:
  explicit MatroskaDemuxer(MediaResource* aResource);

 private:
  nsresult SetVideoCodecInfo(nestegg* aContext, int aTrackId) override;
  nsresult SetContainerAudioCodecInfo(
      nestegg* aContext, const nestegg_audio_params& aParams) override;
  bool CheckKeyFrameByExamineByteStream(const MediaRawData* aSample) override;

  nsresult SetCodecPrivateToVideoExtraData(nestegg* aContext, int aTrackId);
};

}  

#endif
