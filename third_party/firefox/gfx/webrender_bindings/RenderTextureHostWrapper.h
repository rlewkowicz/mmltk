/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_RENDERTEXTUREHOSTWRAPPER_H
#define MOZILLA_GFX_RENDERTEXTUREHOSTWRAPPER_H

#include "RenderTextureHost.h"

namespace mozilla {

namespace wr {

class RenderTextureHostWrapper final : public RenderTextureHost {
 public:
  explicit RenderTextureHostWrapper(ExternalImageId aExternalImageId);

  wr::WrExternalImage Lock(uint8_t aChannelIndex, gl::GLContext* aGL) override;
  void Unlock() override;
  void ClearCachedResources() override;
  void PrepareForUse() override;
  void NotifyForUse() override;
  void NotifyNotUsed() override;
  bool SyncObjectNeeded() override;
  RefPtr<layers::TextureSource> CreateTextureSource(
      layers::TextureSourceProvider* aProvider) override;
  RenderMacIOSurfaceTextureHost* AsRenderMacIOSurfaceTextureHost() override;
  RenderDXGITextureHost* AsRenderDXGITextureHost() override;
  RenderDXGIYCbCrTextureHost* AsRenderDXGIYCbCrTextureHost() override;
  RenderDcompSurfaceTextureHost* AsRenderDcompSurfaceTextureHost() override;
  RenderAndroidHardwareBufferTextureHost*
  AsRenderAndroidHardwareBufferTextureHost() override;
  RenderAndroidImageReaderImageTextureHost*
  AsRenderAndroidImageReaderImageTextureHost() override;
  RenderAndroidSurfaceTextureHost* AsRenderAndroidSurfaceTextureHost() override;
  RenderEGLImageTextureHost* AsRenderEGLImageTextureHost() override;
  RenderDMABUFTextureHost* AsRenderDMABUFTextureHost() override;
  void SetIsSoftwareDecodedVideo() override;
  bool IsSoftwareDecodedVideo() override;
  RefPtr<RenderTextureHostUsageInfo> GetOrMergeUsageInfo(
      const MutexAutoLock& aProofOfMapLock,
      RefPtr<RenderTextureHostUsageInfo> aUsageInfo) override;
  RefPtr<RenderTextureHostUsageInfo> GetTextureHostUsageInfo(
      const MutexAutoLock& aProofOfMapLock) override;

  size_t Bytes() override { return 0; }

 private:
  ~RenderTextureHostWrapper() override;

  void EnsureTextureHost() const;
  ExternalImageId mExternalImageId;
  mutable RefPtr<RenderTextureHost> mTextureHost;
};

}  
}  

#endif  // MOZILLA_GFX_RENDERTEXTUREHOSTWRAPPER_H
