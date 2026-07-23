/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_RENDEREGLIMAGETEXTUREHOST_H
#define MOZILLA_GFX_RENDEREGLIMAGETEXTUREHOST_H

#include "mozilla/layers/TextureHostOGL.h"
#include "RenderTextureHost.h"

namespace mozilla {

namespace wr {

class RenderEGLImageTextureHost final : public RenderTextureHost {
 public:
  RenderEGLImageTextureHost(EGLImage aImage, EGLSync aSync, gfx::IntSize aSize,
                            gfx::SurfaceFormat aFormat);

  wr::WrExternalImage Lock(uint8_t aChannelIndex, gl::GLContext* aGL) override;
  void Unlock() override;
  size_t Bytes() override {
    return mSize.width * mSize.height * BytesPerPixel(mFormat);
  }

  RenderEGLImageTextureHost* AsRenderEGLImageTextureHost() override {
    return this;
  }

  RefPtr<layers::TextureSource> CreateTextureSource(
      layers::TextureSourceProvider* aProvider) override;

  gfx::SurfaceFormat GetFormat() const override;

 private:
  virtual ~RenderEGLImageTextureHost();
  bool CreateTextureHandle();
  void DeleteTextureHandle();
  bool WaitSync();

  const EGLImage mImage;
  EGLSync mSync;
  const gfx::IntSize mSize;
  const gfx::SurfaceFormat mFormat;

  RefPtr<gl::GLContext> mGL;
  GLenum mTextureTarget;
  GLuint mTextureHandle;
};

}  
}  

#endif  // MOZILLA_GFX_RENDEREGLIMAGETEXTUREHOST_H
