/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_RENDEREXTERNALTEXTUREHOST_H
#define MOZILLA_GFX_RENDEREXTERNALTEXTUREHOST_H

#include "mozilla/layers/TextureHostOGL.h"
#include "RenderTextureHost.h"

namespace mozilla {
namespace wr {

class RenderExternalTextureHost final : public RenderTextureHost {
 public:
  RenderExternalTextureHost(uint8_t* aBuffer,
                            const layers::BufferDescriptor& aDescriptor);

  wr::WrExternalImage Lock(uint8_t aChannelIndex, gl::GLContext* aGL) override;
  void Unlock() override;
  void PrepareForUse() override;
  size_t Bytes() override {
    return mSize.width * mSize.height * BytesPerPixel(mFormat);
  }

  gfx::SurfaceFormat GetFormat() const override;

 private:
  ~RenderExternalTextureHost();

  bool CreateSurfaces();
  void DeleteSurfaces();
  void DeleteTextures();

  uint8_t* GetBuffer() const { return mBuffer; }
  bool InitializeIfNeeded();
  bool IsReadyForDeletion();
  bool IsYUV() const { return mFormat == gfx::SurfaceFormat::YUV420; }
  size_t PlaneCount() const { return IsYUV() ? 3 : 1; }
  void UpdateTexture(size_t aIndex);
  void UpdateTextures();

  uint8_t* mBuffer;
  layers::BufferDescriptor mDescriptor;

  bool mInitialized;
  bool mTextureUpdateNeeded;

  gfx::IntSize mSize;
  gfx::SurfaceFormat mFormat;

  RefPtr<gl::GLContext> mGL;
  RefPtr<gfx::DataSourceSurface> mSurfaces[3];
  RefPtr<layers::DirectMapTextureSource> mTextureSources[3];
  wr::WrExternalImage mImages[3];
};

}  
}  

#endif  // MOZILLA_GFX_RENDEREXTERNALTEXTUREHOST_H
