/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_RENDERDMABUFTEXTUREHOST_H
#define MOZILLA_GFX_RENDERDMABUFTEXTUREHOST_H

#include "mozilla/layers/TextureHostOGL.h"
#include "RenderTextureHost.h"
#include "mozilla/widget/DMABufSurface.h"

namespace mozilla {

namespace layers {
class SurfaceDescriptorDMABuf;
}

namespace wr {

class RenderDMABUFTextureHost final : public RenderTextureHost {
 public:
  explicit RenderDMABUFTextureHost(DMABufSurface* aSurface);

  RenderDMABUFTextureHost* AsRenderDMABUFTextureHost() override { return this; }
  gfx::IntSize GetSize(uint8_t aChannelIndex) const;
  wr::WrExternalImage Lock(uint8_t aChannelIndex, gl::GLContext* aGL) override;
  void Unlock() override;
  void ClearCachedResources() override;

  size_t Bytes() override {
    return mSurface->GetWidth() * mSurface->GetHeight() *
           BytesPerPixel(mSurface->GetFormat());
  }
  RefPtr<DMABufSurface> GetSurface() { return mSurface; }

  gfx::SurfaceFormat GetFormat() const override {
    return mSurface->GetFormat();
  }

 private:
  virtual ~RenderDMABUFTextureHost();
  void DeleteTextureHandle();

  RefPtr<DMABufSurface> mSurface;
  RefPtr<gl::GLContext> mGL;
};

}  
}  

#endif  // MOZILLA_GFX_RENDERDMABUFTEXTUREHOST_H
