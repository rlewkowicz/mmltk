/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_COMPOSITINGRENDERTARGETOGL_H
#define MOZILLA_GFX_COMPOSITINGRENDERTARGETOGL_H

#include "GLContextTypes.h"             // for GLContext
#include "GLDefs.h"                     // for GLenum, LOCAL_GL_FRAMEBUFFER, etc
#include "mozilla/Assertions.h"         // for MOZ_ASSERT, etc
#include "mozilla/RefPtr.h"             // for RefPtr, already_AddRefed
#include "mozilla/gfx/Point.h"          // for IntSize, IntSizeTyped
#include "mozilla/gfx/Types.h"          // for SurfaceFormat, etc
#include "mozilla/layers/Compositor.h"  // for SurfaceInitMode, etc
#include "mozilla/layers/TextureHost.h"    // for CompositingRenderTarget
#include "mozilla/layers/CompositorOGL.h"  // for CompositorOGL
#include "mozilla/mozalloc.h"              // for operator new
#include "nsAString.h"
#include "nsCOMPtr.h"  // for already_AddRefed
#include "nsDebug.h"   // for NS_ERROR, NS_WARNING
#include "nsString.h"  // for nsAutoCString

namespace mozilla {
namespace gl {
class BindableTexture;
}  
namespace gfx {
class DataSourceSurface;
}  

namespace layers {

class TextureSource;

class CompositingRenderTargetOGL : public CompositingRenderTarget {
  typedef mozilla::gl::GLContext GLContext;

  friend class CompositorOGL;

  enum class GLResourceOwnership : uint8_t {
    OWNED_BY_RENDER_TARGET,

    EXTERNALLY_OWNED
  };

  struct InitParams {
    GLenum mFBOTextureTarget;
    SurfaceInitMode mInitMode;
  };

 public:
  ~CompositingRenderTargetOGL();

  const char* Name() const override { return "CompositingRenderTargetOGL"; }

  static already_AddRefed<CompositingRenderTargetOGL> CreateForWindow(
      CompositorOGL* aCompositor, const gfx::IntSize& aSize) {
    RefPtr<CompositingRenderTargetOGL> result = new CompositingRenderTargetOGL(
        aCompositor, gfx::IntRect(gfx::IntPoint(), aSize), gfx::IntPoint(),
        aSize, GLResourceOwnership::EXTERNALLY_OWNED, 0, 0, Nothing());
    return result.forget();
  }

  static already_AddRefed<CompositingRenderTargetOGL>
  CreateForNewFBOAndTakeOwnership(CompositorOGL* aCompositor, GLuint aTexture,
                                  GLuint aFBO, const gfx::IntRect& aRect,
                                  const gfx::IntPoint& aClipSpaceOrigin,
                                  const gfx::IntSize& aPhySize,
                                  GLenum aFBOTextureTarget,
                                  SurfaceInitMode aInit) {
    RefPtr<CompositingRenderTargetOGL> result = new CompositingRenderTargetOGL(
        aCompositor, aRect, aClipSpaceOrigin, aPhySize,
        GLResourceOwnership::OWNED_BY_RENDER_TARGET, aTexture, aFBO,
        Some(InitParams{aFBOTextureTarget, aInit}));
    return result.forget();
  }

  static already_AddRefed<CompositingRenderTargetOGL>
  CreateForExternallyOwnedFBO(CompositorOGL* aCompositor, GLuint aFBO,
                              const gfx::IntRect& aRect,
                              const gfx::IntPoint& aClipSpaceOrigin) {
    RefPtr<CompositingRenderTargetOGL> result = new CompositingRenderTargetOGL(
        aCompositor, aRect, aClipSpaceOrigin, aRect.Size(),
        GLResourceOwnership::EXTERNALLY_OWNED, 0, aFBO, Nothing());
    return result.forget();
  }

  void BindTexture(GLenum aTextureUnit, GLenum aTextureTarget);

  void BindRenderTarget();

  bool IsWindow() { return mFBO == 0; }

  GLuint GetFBO() const;

  GLuint GetTextureHandle() const {
    MOZ_ASSERT(!mNeedInitialization);
    return mTextureHandle;
  }

  TextureSourceOGL* AsSourceOGL() override {
    MOZ_ASSERT(
        false,
        "CompositingRenderTargetOGL should not be used as a TextureSource");
    return nullptr;
  }
  gfx::IntSize GetSize() const override { return mSize; }

  gfx::IntPoint GetClipSpaceOrigin() const { return mClipSpaceOrigin; }

  gfx::SurfaceFormat GetFormat() const override {
    MOZ_ASSERT(false, "Not implemented");
    return gfx::SurfaceFormat::UNKNOWN;
  }

  void SetClipRect(const Maybe<gfx::IntRect>& aRect) { mClipRect = aRect; }
  const Maybe<gfx::IntRect>& GetClipRect() const { return mClipRect; }

#ifdef MOZ_DUMP_PAINTING
  already_AddRefed<gfx::DataSourceSurface> Dump(
      Compositor* aCompositor) override;
#endif

  const gfx::IntSize& GetInitSize() const { return mSize; }
  const gfx::IntSize& GetPhysicalSize() const { return mPhySize; }

 protected:
  CompositingRenderTargetOGL(CompositorOGL* aCompositor,
                             const gfx::IntRect& aRect,
                             const gfx::IntPoint& aClipSpaceOrigin,
                             const gfx::IntSize& aPhySize,
                             GLResourceOwnership aGLResourceOwnership,
                             GLuint aTexure, GLuint aFBO,
                             const Maybe<InitParams>& aNeedInitialization)
      : CompositingRenderTarget(aRect.TopLeft()),
        mNeedInitialization(aNeedInitialization),
        mSize(aRect.Size()),
        mPhySize(aPhySize),
        mCompositor(aCompositor),
        mGL(aCompositor->gl()),
        mClipSpaceOrigin(aClipSpaceOrigin),
        mGLResourceOwnership(aGLResourceOwnership),
        mTextureHandle(aTexure),
        mFBO(aFBO) {
    MOZ_ASSERT(mGL);
  }

  void Initialize(GLenum aFBOTextureTarget);

  Maybe<InitParams> mNeedInitialization;

  gfx::IntSize mSize;     
  gfx::IntSize mPhySize;  

  RefPtr<CompositorOGL> mCompositor;
  RefPtr<GLContext> mGL;
  Maybe<gfx::IntRect> mClipRect;
  gfx::IntPoint mClipSpaceOrigin;
  GLResourceOwnership mGLResourceOwnership;
  GLuint mTextureHandle;
  GLuint mFBO;
};

}  
}  

#endif /* MOZILLA_GFX_SURFACEOGL_H */
