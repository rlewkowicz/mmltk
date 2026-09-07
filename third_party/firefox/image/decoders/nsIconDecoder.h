/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_image_decoders_nsIconDecoder_h
#define mozilla_image_decoders_nsIconDecoder_h

#include "Decoder.h"
#include "StreamingLexer.h"
#include "SurfacePipe.h"

namespace mozilla {
namespace image {

class RasterImage;


class nsIconDecoder : public Decoder {
 public:
  virtual ~nsIconDecoder();

  DecoderType GetType() const override { return DecoderType::ICON; }

  LexerResult DoDecode(SourceBufferIterator& aIterator,
                       IResumable* aOnResume) override;

 private:
  friend class DecoderFactory;

  explicit nsIconDecoder(RasterImage* aImage);

  enum class State { HEADER, ROW_OF_PIXELS, FINISH };

  LexerTransition<State> ReadHeader(const char* aData);
  LexerTransition<State> ReadRowOfPixels(const char* aData, size_t aLength);
  LexerTransition<State> Finish();

  StreamingLexer<State> mLexer;
  SurfacePipe mPipe;
  uint32_t mBytesPerRow;
};

}  
}  

#endif  // mozilla_image_decoders_nsIconDecoder_h
