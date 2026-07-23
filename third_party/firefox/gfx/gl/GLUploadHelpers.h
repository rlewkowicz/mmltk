/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GLUploadHelpers_h_
#define GLUploadHelpers_h_

#include "GLDefs.h"
#include "mozilla/gfx/Types.h"
#include "nsPoint.h"
#include "nsRegionFwd.h"

namespace mozilla {

namespace gfx {
class DataSourceSurface;
}  

namespace gl {

class GLContext;

gfx::SurfaceFormat UploadImageDataToTexture(
    GLContext* gl, unsigned char* aData, const gfx::IntSize& aDataSize,
    const gfx::IntPoint& aDstOffset, int32_t aStride,
    gfx::SurfaceFormat aFormat, const nsIntRegion& aDstRegion, GLuint aTexture,
    const gfx::IntSize& aSize, size_t* aOutUploadSize = nullptr,
    bool aNeedInit = false, GLenum aTextureUnit = LOCAL_GL_TEXTURE0,
    GLenum aTextureTarget = LOCAL_GL_TEXTURE_2D);

gfx::SurfaceFormat UploadSurfaceToTexture(
    GLContext* gl, gfx::DataSourceSurface* aSurface,
    const nsIntRegion& aDstRegion, GLuint aTexture, const gfx::IntSize& aSize,
    size_t* aOutUploadSize = nullptr, bool aNeedInit = false,
    const gfx::IntPoint& aSrcOffset = gfx::IntPoint(0, 0),
    const gfx::IntPoint& aDstOffset = gfx::IntPoint(0, 0),
    GLenum aTextureUnit = LOCAL_GL_TEXTURE0,
    GLenum aTextureTarget = LOCAL_GL_TEXTURE_2D);

bool ShouldUploadSubTextures(GLContext* gl);
bool CanUploadNonPowerOfTwo(GLContext* gl);

}  
}  

#endif
