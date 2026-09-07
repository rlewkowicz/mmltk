/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(GFX_GLIMAGES_H)
#define GFX_GLIMAGES_H

#include "GLContextTypes.h"
#include "GLTypes.h"
#include "ImageContainer.h"      // for Image
#include "ImageTypes.h"          // for ImageFormat::SHARED_GLTEXTURE
#include "nsCOMPtr.h"            // for already_AddRefed
#include "mozilla/Maybe.h"       // for Maybe
#include "mozilla/gfx/Matrix.h"  // for Matrix4x4
#include "mozilla/gfx/Point.h"   // for IntSize


namespace mozilla {
namespace layers {

class GLImage : public Image {
 public:
  explicit GLImage(ImageFormat aFormat) : Image(nullptr, aFormat) {}

  already_AddRefed<gfx::SourceSurface> GetAsSourceSurface() override;

  nsresult BuildSurfaceDescriptorBuffer(
      SurfaceDescriptorBuffer& aSdBuffer, BuildSdbFlags aFlags,
      const std::function<MemoryOrShmem(uint32_t)>& aAllocate) override;

  GLImage* AsGLImage() override { return this; }

 protected:
  nsresult ReadIntoBuffer(uint8_t* aData, int32_t aStride,
                          const gfx::IntSize& aSize,
                          gfx::SurfaceFormat aFormat);
};


}  
}  

#endif
