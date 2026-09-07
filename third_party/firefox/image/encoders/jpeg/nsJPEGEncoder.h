/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_image_encoders_jpeg_nsJPEGEncoder_h
#define mozilla_image_encoders_jpeg_nsJPEGEncoder_h

#include "imgIEncoder.h"
#include "mozilla/Attributes.h"
#include "mozilla/ReentrantMonitor.h"
#include "nsCOMPtr.h"

struct jpeg_compress_struct;
struct jpeg_common_struct;

#define NS_JPEGENCODER_CID                    \
  { \
   0xac2bb8fe,                                \
   0xeeeb,                                    \
   0x4572,                                    \
   {0xb4, 0x0f, 0xbe, 0x03, 0x93, 0x2b, 0x56, 0xe0}}

class nsJPEGEncoderInternal;

class nsJPEGEncoder final : public imgIEncoder {
  friend class nsJPEGEncoderInternal;
  typedef mozilla::ReentrantMonitor ReentrantMonitor;

 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_IMGIENCODER
  NS_DECL_NSIINPUTSTREAM
  NS_DECL_NSIASYNCINPUTSTREAM

  nsJPEGEncoder();

 private:
  ~nsJPEGEncoder();

 protected:
  void ConvertHostARGBRow(const uint8_t* aSrc, uint8_t* aDest,
                          uint32_t aPixelWidth);
  void ConvertRGBARow(const uint8_t* aSrc, uint8_t* aDest,
                      uint32_t aPixelWidth);

  void NotifyListener();

  bool mFinished;

  uint8_t* mImageBuffer;
  uint32_t mImageBufferSize;
  uint32_t mImageBufferUsed;

  uint32_t mImageBufferReadPoint;

  nsCOMPtr<nsIInputStreamCallback> mCallback;
  nsCOMPtr<nsIEventTarget> mCallbackTarget;
  uint32_t mNotifyThreshold;

  ReentrantMonitor mReentrantMonitor MOZ_UNANNOTATED;
};

#endif  // mozilla_image_encoders_jpeg_nsJPEGEncoder_h
