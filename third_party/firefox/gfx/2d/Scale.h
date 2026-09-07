/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_SCALE_H_
#define MOZILLA_GFX_SCALE_H_

#include "Types.h"

namespace mozilla {
namespace gfx {

GFX2D_API bool Scale(uint8_t* srcData, int32_t srcWidth, int32_t srcHeight,
                     int32_t srcStride, uint8_t* dstData, int32_t dstWidth,
                     int32_t dstHeight, int32_t dstStride, SurfaceFormat format,
                     SamplingFilter aFilter = SamplingFilter::GOOD);

}  
}  

#endif /* MOZILLA_GFX_BLUR_H_ */
