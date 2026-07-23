/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_image_ImgDrawResult_h
#define mozilla_image_ImgDrawResult_h

#include <cstdint>  // for uint8_t

#include "mozilla/Likely.h"

namespace mozilla {
namespace image {

enum class [[nodiscard]] ImgDrawResult : uint8_t {
  SUCCESS,
  SUCCESS_NOT_COMPLETE,
  INCOMPLETE,
  WRONG_SIZE,
  NOT_READY,
  TEMPORARY_ERROR,
  BAD_IMAGE,
  BAD_ARGS,
  NOT_SUPPORTED
};

inline ImgDrawResult operator&(const ImgDrawResult aLeft,
                               const ImgDrawResult aRight) {
  if (MOZ_LIKELY(aLeft == ImgDrawResult::SUCCESS)) {
    return aRight;
  }

  if (aLeft == ImgDrawResult::NOT_SUPPORTED ||
      aRight == ImgDrawResult::NOT_SUPPORTED) {
    return ImgDrawResult::NOT_SUPPORTED;
  }

  if ((aLeft == ImgDrawResult::BAD_IMAGE ||
       aLeft == ImgDrawResult::SUCCESS_NOT_COMPLETE) &&
      aRight != ImgDrawResult::SUCCESS &&
      aRight != ImgDrawResult::SUCCESS_NOT_COMPLETE) {
    return aRight;
  }
  return aLeft;
}

inline ImgDrawResult& operator&=(ImgDrawResult& aLeft,
                                 const ImgDrawResult aRight) {
  aLeft = aLeft & aRight;
  return aLeft;
}

struct imgDrawingParams {
  explicit imgDrawingParams(uint32_t aImageFlags = 0)
      : imageFlags(aImageFlags), result(ImgDrawResult::SUCCESS) {}

  const uint32_t imageFlags;  
  ImgDrawResult result;       
};

}  
}  

#endif  // mozilla_image_ImgDrawResult_h
