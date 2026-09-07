/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(MOZILLA_GFX_TEXTUREOGL_H)
#define MOZILLA_GFX_TEXTUREOGL_H

#include <stddef.h>  // for size_t
#include <stdint.h>  // for uint64_t
#include "CompositableHost.h"
#include "GLContextTypes.h"  // for GLContext
#include "GLDefs.h"          // for GLenum, LOCAL_GL_CLAMP_TO_EDGE, etc
#include "GLTextureImage.h"  // for TextureImage
#include "gfxTypes.h"
#include "mozilla/GfxMessageUtils.h"         // for gfxContentType
#include "mozilla/Assertions.h"              // for MOZ_ASSERT, etc
#include "mozilla/RefPtr.h"                  // for RefPtr
#include "mozilla/gfx/Matrix.h"              // for Matrix4x4
#include "mozilla/gfx/Point.h"               // for IntSize, IntPoint
#include "mozilla/gfx/Types.h"               // for SurfaceFormat, etc
#include "mozilla/layers/CompositorOGL.h"    // for CompositorOGL
#include "mozilla/layers/CompositorTypes.h"  // for TextureFlags
#include "mozilla/layers/LayersSurfaces.h"   // for SurfaceDescriptor
#include "mozilla/layers/TextureHost.h"      // for TextureHost, etc
#include "mozilla/mozalloc.h"                // for operator delete, etc
#include "mozilla/webrender/RenderThread.h"
#include "nsCOMPtr.h"         // for already_AddRefed
#include "nsDebug.h"          // for NS_WARNING
#include "nsISupportsImpl.h"  // for TextureImage::Release, etc
#include "nsRegionFwd.h"      // for nsIntRegion


namespace mozilla {
namespace gfx {
class DataSourceSurface;
}  

namespace layers {

class Compositor;
class CompositorOGL;
class AndroidHardwareBuffer;
class AndroidImageConsumer;
class SurfaceDescriptorAndroidHardwareBuffer;
class TextureImageTextureSourceOGL;
class GLTextureSource;

void ApplySamplingFilterToBoundTexture(gl::GLContext* aGL,
                                       gfx::SamplingFilter aSamplingFilter,
                                       GLuint aTarget = LOCAL_GL_TEXTURE_2D);

already_AddRefed<TextureHost> CreateTextureHostOGL(
    const SurfaceDescriptor& aDesc, ISurfaceAllocator* aDeallocator,
    LayersBackend aBackend, TextureFlags aFlags);


class TextureSourceOGL {
 public:
  TextureSourceOGL()
      : mCachedSamplingFilter(gfx::SamplingFilter::GOOD),
        mHasCachedSamplingFilter(false) {}

  virtual bool IsValid() const = 0;

  virtual void BindTexture(GLenum aTextureUnit,
                           gfx::SamplingFilter aSamplingFilter) = 0;

  virtual void MaybeFenceTexture() {}

  virtual gfx::IntSize GetSize() const = 0;

  virtual GLenum GetTextureTarget() const { return LOCAL_GL_TEXTURE_2D; }

  virtual gfx::SurfaceFormat GetFormat() const = 0;

  virtual GLenum GetWrapMode() const = 0;

  virtual gfx::Matrix4x4 GetTextureTransform() { return gfx::Matrix4x4(); }

  virtual TextureImageTextureSourceOGL* AsTextureImageTextureSource() {
    return nullptr;
  }

  virtual GLTextureSource* AsGLTextureSource() { return nullptr; }

  void SetSamplingFilter(gl::GLContext* aGL,
                         gfx::SamplingFilter aSamplingFilter) {
    if (mHasCachedSamplingFilter && mCachedSamplingFilter == aSamplingFilter) {
      return;
    }
    mHasCachedSamplingFilter = true;
    mCachedSamplingFilter = aSamplingFilter;
    ApplySamplingFilterToBoundTexture(aGL, aSamplingFilter, GetTextureTarget());
  }

  void ClearCachedFilter() { mHasCachedSamplingFilter = false; }

 private:
  gfx::SamplingFilter mCachedSamplingFilter;
  bool mHasCachedSamplingFilter;
};

class TextureImageTextureSourceOGL final : public DataTextureSource,
                                           public TextureSourceOGL,
                                           public BigImageIterator {
 public:
  explicit TextureImageTextureSourceOGL(
      CompositorOGL* aCompositor, TextureFlags aFlags = TextureFlags::DEFAULT);

  const char* Name() const override { return "TextureImageTextureSourceOGL"; }

  bool Update(gfx::DataSourceSurface* aSurface,
              nsIntRegion* aDestRegion = nullptr,
              gfx::IntPoint* aSrcOffset = nullptr,
              gfx::IntPoint* aDstOffset = nullptr) override;

  void EnsureBuffer(const gfx::IntSize& aSize, gfxContentType aContentType);

  TextureImageTextureSourceOGL* AsTextureImageTextureSource() override {
    return this;
  }


  void DeallocateDeviceData() override;

  TextureSourceOGL* AsSourceOGL() override { return this; }

  void BindTexture(GLenum aTextureUnit,
                   gfx::SamplingFilter aSamplingFilter) override;

  gfx::IntSize GetSize() const override;

  gfx::SurfaceFormat GetFormat() const override;

  bool IsValid() const override { return !!mTexImage; }

  GLenum GetWrapMode() const override { return mTexImage->GetWrapMode(); }


  BigImageIterator* AsBigImageIterator() override { return this; }

  void BeginBigImageIteration() override {
    mTexImage->BeginBigImageIteration();
    mIterating = true;
  }

  void EndBigImageIteration() override { mIterating = false; }

  gfx::IntRect GetTileRect() override;

  size_t GetTileCount() override { return mTexImage->GetTileCount(); }

  bool NextTile() override { return mTexImage->NextTile(); }

  gl::GLContext* gl() const { return mGL; }

 protected:
  ~TextureImageTextureSourceOGL();

  RefPtr<gl::TextureImage> mTexImage;
  RefPtr<gl::GLContext> mGL;
  RefPtr<CompositorOGL> mCompositor;
  TextureFlags mFlags;
  bool mIterating;
};

class GLTextureSource : public DataTextureSource, public TextureSourceOGL {
 public:
  GLTextureSource(TextureSourceProvider* aProvider, GLuint aTextureHandle,
                  GLenum aTarget, gfx::IntSize aSize,
                  gfx::SurfaceFormat aFormat);

  GLTextureSource(gl::GLContext* aGL, GLuint aTextureHandle, GLenum aTarget,
                  gfx::IntSize aSize, gfx::SurfaceFormat aFormat);

  virtual ~GLTextureSource();

  const char* Name() const override { return "GLTextureSource"; }

  GLTextureSource* AsGLTextureSource() override { return this; }

  TextureSourceOGL* AsSourceOGL() override { return this; }

  void BindTexture(GLenum activetex,
                   gfx::SamplingFilter aSamplingFilter) override;

  bool IsValid() const override;

  gfx::IntSize GetSize() const override { return mSize; }

  gfx::SurfaceFormat GetFormat() const override { return mFormat; }

  GLenum GetTextureTarget() const override { return mTextureTarget; }

  GLenum GetWrapMode() const override { return LOCAL_GL_CLAMP_TO_EDGE; }

  void DeallocateDeviceData() override;

  void SetSize(gfx::IntSize aSize) { mSize = aSize; }

  void SetFormat(gfx::SurfaceFormat aFormat) { mFormat = aFormat; }

  GLuint GetTextureHandle() const { return mTextureHandle; }

  gl::GLContext* gl() const { return mGL; }

  bool Update(gfx::DataSourceSurface* aSurface,
              nsIntRegion* aDestRegion = nullptr,
              gfx::IntPoint* aSrcOffset = nullptr,
              gfx::IntPoint* aDstOffset = nullptr) override {
    return false;
  }

 protected:
  void DeleteTextureHandle();

  RefPtr<gl::GLContext> mGL;
  RefPtr<CompositorOGL> mCompositor;
  GLuint mTextureHandle;
  GLenum mTextureTarget;
  gfx::IntSize mSize;
  gfx::SurfaceFormat mFormat;
};

class DirectMapTextureSource : public GLTextureSource {
 public:
  DirectMapTextureSource(gl::GLContext* aContext,
                         gfx::DataSourceSurface* aSurface);
  DirectMapTextureSource(TextureSourceProvider* aProvider,
                         gfx::DataSourceSurface* aSurface);
  ~DirectMapTextureSource();

  bool Update(gfx::DataSourceSurface* aSurface,
              nsIntRegion* aDestRegion = nullptr,
              gfx::IntPoint* aSrcOffset = nullptr,
              gfx::IntPoint* aDstOffset = nullptr) override;

  bool Sync(bool aBlocking) override;

  void MaybeFenceTexture() override;

 private:
  bool UpdateInternal(gfx::DataSourceSurface* aSurface,
                      nsIntRegion* aDestRegion, gfx::IntPoint* aSrcOffset,
                      bool aInit);

  GLsync mSync;
};

class GLTextureHost : public TextureHost {
 public:
  GLTextureHost(TextureFlags aFlags, GLuint aTextureHandle, GLenum aTarget,
                GLsync aSync, gfx::IntSize aSize, bool aHasAlpha);

  virtual ~GLTextureHost();

  void DeallocateDeviceData() override {}

  gfx::SurfaceFormat GetFormat() const override;

  already_AddRefed<gfx::DataSourceSurface> GetAsSurface(
      gfx::DataSourceSurface* aSurface) override {
    return nullptr;  
  }

  gl::GLContext* gl() const;

  gfx::IntSize GetSize() const override { return mSize; }

  const char* Name() override { return "GLTextureHost"; }

 protected:
  const GLuint mTexture;
  const GLenum mTarget;
  GLsync mSync;
  const gfx::IntSize mSize;
  const bool mHasAlpha;
  RefPtr<GLTextureSource> mTextureSource;
};




class EGLImageTextureSource : public TextureSource, public TextureSourceOGL {
 public:
  EGLImageTextureSource(TextureSourceProvider* aProvider, EGLImage aImage,
                        gfx::SurfaceFormat aFormat, GLenum aTarget,
                        GLenum aWrapMode, gfx::IntSize aSize);

  const char* Name() const override { return "EGLImageTextureSource"; }

  TextureSourceOGL* AsSourceOGL() override { return this; }

  void BindTexture(GLenum activetex,
                   gfx::SamplingFilter aSamplingFilter) override;

  bool IsValid() const override;

  gfx::IntSize GetSize() const override { return mSize; }

  gfx::SurfaceFormat GetFormat() const override { return mFormat; }

  gfx::Matrix4x4 GetTextureTransform() override;

  GLenum GetTextureTarget() const override { return mTextureTarget; }

  GLenum GetWrapMode() const override { return mWrapMode; }

  void DeallocateDeviceData() override {}

  gl::GLContext* gl() const { return mGL; }

 protected:
  RefPtr<gl::GLContext> mGL;
  RefPtr<CompositorOGL> mCompositor;
  const EGLImage mImage;
  const gfx::SurfaceFormat mFormat;
  const GLenum mTextureTarget;
  const GLenum mWrapMode;
  const gfx::IntSize mSize;
};

class EGLImageTextureHost final : public TextureHost {
 public:
  EGLImageTextureHost(TextureFlags aFlags, EGLImage aImage, EGLSync aSync,
                      gfx::IntSize aSize, bool hasAlpha);

  virtual ~EGLImageTextureHost();

  void DeallocateDeviceData() override {}

  gfx::SurfaceFormat GetFormat() const override;

  already_AddRefed<gfx::DataSourceSurface> GetAsSurface(
      gfx::DataSourceSurface* aSurface) override {
    return nullptr;  
  }

  gl::GLContext* gl() const;

  gfx::IntSize GetSize() const override { return mSize; }

  const char* Name() override { return "EGLImageTextureHost"; }

  void CreateRenderTexture(
      const wr::ExternalImageId& aExternalImageId) override;

  void PushResourceUpdates(wr::TransactionBuilder& aResources,
                           ResourceUpdateOp aOp,
                           const Range<wr::ImageKey>& aImageKeys,
                           const wr::ExternalImageId& aExtID) override;

  void PushDisplayItems(wr::DisplayListBuilder& aBuilder,
                        const wr::LayoutRect& aBounds,
                        const wr::LayoutRect& aClip, wr::ImageRendering aFilter,
                        const Range<wr::ImageKey>& aImageKeys,
                        PushDisplayItemFlagSet aFlags) override;

  bool SupportsExternalCompositing(WebRenderBackend aBackend) override;

 protected:
  const EGLImage mImage;
  const EGLSync mSync;
  const gfx::IntSize mSize;
  const bool mHasAlpha;
  RefPtr<EGLImageTextureSource> mTextureSource;
};

}  
}  

#endif
