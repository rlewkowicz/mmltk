/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_image_PlaybackType_h
#define mozilla_image_PlaybackType_h

#include "imgIContainer.h"

namespace mozilla {
namespace image {

enum class PlaybackType : uint8_t {
  eStatic,   
  eAnimated  
};

inline PlaybackType ToPlaybackType(uint32_t aWhichFrame) {
  MOZ_ASSERT(aWhichFrame == imgIContainer::FRAME_FIRST ||
             aWhichFrame == imgIContainer::FRAME_CURRENT);
  return aWhichFrame == imgIContainer::FRAME_CURRENT ? PlaybackType::eAnimated
                                                     : PlaybackType::eStatic;
}

}  
}  

#endif  // mozilla_image_PlaybackType_h
