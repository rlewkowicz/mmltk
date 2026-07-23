/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_image_DecoderFlags_h
#define mozilla_image_DecoderFlags_h

#include "mozilla/TypedEnumBits.h"

namespace mozilla {
namespace image {

enum class DecoderFlags : uint8_t {
  FIRST_FRAME_ONLY = 1 << 0,
  IS_REDECODE = 1 << 1,
  IMAGE_IS_TRANSIENT = 1 << 2,
  ASYNC_NOTIFY = 1 << 3,

  CANNOT_SUBSTITUTE = 1 << 4,

  COUNT_FRAMES = 1 << 5,
};
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(DecoderFlags)

inline DecoderFlags DefaultDecoderFlags() { return DecoderFlags(); }

}  
}  

#endif  // mozilla_image_DecoderFlags_h
