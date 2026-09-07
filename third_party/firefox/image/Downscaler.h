/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_image_Downscaler_h
#define mozilla_image_Downscaler_h

#include "gfxPoint.h"
#include "mozilla/Maybe.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/gfx/ConvolutionFilter.h"
#include "mozilla/gfx/Matrix.h"
#include "nsRect.h"

namespace mozilla {
namespace image {

struct DownscalerInvalidRect {
  nsIntRect mOriginalSizeRect;
  nsIntRect mTargetSizeRect;
};

class Downscaler {
 public:
  explicit Downscaler(const nsIntSize& aTargetSize);
  ~Downscaler();

  const nsIntSize& OriginalSize() const { return mOriginalSize; }
  const nsIntSize& TargetSize() const { return mTargetSize; }
  const nsIntSize FrameSize() const {
    return nsIntSize(mFrameRect.Width(), mFrameRect.Height());
  }
  const gfx::MatrixScalesDouble& Scale() const { return mScale; }

  nsresult BeginFrame(const nsIntSize& aOriginalSize,
                      const Maybe<nsIntRect>& aFrameRect,
                      uint8_t* aOutputBuffer, gfx::SurfaceFormat aFormat,
                      bool aFlipVertically = false);

  bool IsFrameComplete() const {
    return mCurrentInLine >= mOriginalSize.height;
  }

  uint8_t* RowBuffer() {
    return mRowBuffer.get() + mFrameRect.X() * sizeof(uint32_t);
  }

  void ClearRow() { ClearRestOfRow(0); }

  void ClearRestOfRow(uint32_t aStartingAtCol);

  void CommitRow();

  bool HasInvalidation() const;

  DownscalerInvalidRect TakeInvalidRect();

  void ResetForNextProgressivePass();

 private:
  void DownscaleInputLine();
  void ReleaseWindow();
  void SkipToRow(int32_t aRow);

  nsIntSize mOriginalSize;
  nsIntSize mTargetSize;
  nsIntRect mFrameRect;
  gfx::MatrixScalesDouble mScale;

  uint8_t* mOutputBuffer;

  UniquePtr<uint8_t[]> mRowBuffer;
  UniquePtr<uint8_t*[]> mWindow;

  gfx::ConvolutionFilter mXFilter;
  gfx::ConvolutionFilter mYFilter;

  int32_t mWindowCapacity;

  int32_t mLinesInBuffer;
  int32_t mPrevInvalidatedLine;
  int32_t mCurrentOutLine;
  int32_t mCurrentInLine;

  gfx::SurfaceFormat mFormat;
  bool mFlipVertically : 1;
};

}  
}  

#endif  // mozilla_image_Downscaler_h
