/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RenderTextureHostWrapper.h"

#include "mozilla/gfx/Logging.h"
#include "mozilla/layers/RemoteTextureMap.h"
#include "mozilla/webrender/RenderThread.h"

namespace mozilla {
namespace wr {

RenderTextureHostWrapper::RenderTextureHostWrapper(
    ExternalImageId aExternalImageId)
    : mExternalImageId(aExternalImageId) {
  MOZ_COUNT_CTOR_INHERITED(RenderTextureHostWrapper, RenderTextureHost);
  EnsureTextureHost();
}

RenderTextureHostWrapper::~RenderTextureHostWrapper() {
  MOZ_COUNT_DTOR_INHERITED(RenderTextureHostWrapper, RenderTextureHost);
}

void RenderTextureHostWrapper::EnsureTextureHost() const {
  if (mTextureHost) {
    return;
  }

  mTextureHost = RenderThread::Get()->GetRenderTexture(mExternalImageId);
  MOZ_ASSERT(mTextureHost);
  if (!mTextureHost) {
    gfxCriticalNoteOnce << "Failed to get RenderTextureHost for extId:"
                        << AsUint64(mExternalImageId);
  }
}

wr::WrExternalImage RenderTextureHostWrapper::Lock(uint8_t aChannelIndex,
                                                   gl::GLContext* aGL) {
  if (!mTextureHost) {
    return InvalidToWrExternalImage();
  }

  return mTextureHost->Lock(aChannelIndex, aGL);
}

void RenderTextureHostWrapper::Unlock() {
  if (mTextureHost) {
    mTextureHost->Unlock();
  }
}

void RenderTextureHostWrapper::ClearCachedResources() {
  if (mTextureHost) {
    mTextureHost->ClearCachedResources();
  }
}

void RenderTextureHostWrapper::PrepareForUse() {
  if (!mTextureHost) {
    return;
  }
  mTextureHost->PrepareForUse();
}

void RenderTextureHostWrapper::NotifyForUse() {
  if (!mTextureHost) {
    return;
  }
  mTextureHost->NotifyForUse();
}

void RenderTextureHostWrapper::NotifyNotUsed() {
  if (!mTextureHost) {
    return;
  }
  mTextureHost->NotifyNotUsed();
}

bool RenderTextureHostWrapper::SyncObjectNeeded() { return false; }

RefPtr<layers::TextureSource> RenderTextureHostWrapper::CreateTextureSource(
    layers::TextureSourceProvider* aProvider) {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->CreateTextureSource(aProvider);
}

RenderMacIOSurfaceTextureHost*
RenderTextureHostWrapper::AsRenderMacIOSurfaceTextureHost() {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->AsRenderMacIOSurfaceTextureHost();
}

RenderDXGITextureHost* RenderTextureHostWrapper::AsRenderDXGITextureHost() {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->AsRenderDXGITextureHost();
}

RenderDXGIYCbCrTextureHost*
RenderTextureHostWrapper::AsRenderDXGIYCbCrTextureHost() {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->AsRenderDXGIYCbCrTextureHost();
}

RenderDcompSurfaceTextureHost*
RenderTextureHostWrapper::AsRenderDcompSurfaceTextureHost() {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->AsRenderDcompSurfaceTextureHost();
}

RenderDMABUFTextureHost* RenderTextureHostWrapper::AsRenderDMABUFTextureHost() {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->AsRenderDMABUFTextureHost();
}

RenderAndroidHardwareBufferTextureHost*
RenderTextureHostWrapper::AsRenderAndroidHardwareBufferTextureHost() {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->AsRenderAndroidHardwareBufferTextureHost();
}

RenderAndroidImageReaderImageTextureHost*
RenderTextureHostWrapper::AsRenderAndroidImageReaderImageTextureHost() {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->AsRenderAndroidImageReaderImageTextureHost();
}

RenderAndroidSurfaceTextureHost*
RenderTextureHostWrapper::AsRenderAndroidSurfaceTextureHost() {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->AsRenderAndroidSurfaceTextureHost();
}

RenderEGLImageTextureHost*
RenderTextureHostWrapper::AsRenderEGLImageTextureHost() {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->AsRenderEGLImageTextureHost();
}

void RenderTextureHostWrapper::SetIsSoftwareDecodedVideo() {
  if (!mTextureHost) {
    return;
  }
  return mTextureHost->SetIsSoftwareDecodedVideo();
}

bool RenderTextureHostWrapper::IsSoftwareDecodedVideo() {
  if (!mTextureHost) {
    return false;
  }
  return mTextureHost->IsSoftwareDecodedVideo();
}

RefPtr<RenderTextureHostUsageInfo>
RenderTextureHostWrapper::GetOrMergeUsageInfo(
    const MutexAutoLock& aProofOfMapLock,
    RefPtr<RenderTextureHostUsageInfo> aUsageInfo) {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->GetOrMergeUsageInfo(aProofOfMapLock, aUsageInfo);
}

RefPtr<RenderTextureHostUsageInfo>
RenderTextureHostWrapper::GetTextureHostUsageInfo(
    const MutexAutoLock& aProofOfMapLock) {
  if (!mTextureHost) {
    return nullptr;
  }
  return mTextureHost->GetTextureHostUsageInfo(aProofOfMapLock);
}

}  
}  
