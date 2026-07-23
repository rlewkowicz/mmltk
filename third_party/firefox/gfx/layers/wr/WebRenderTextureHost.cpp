/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebRenderTextureHost.h"

#include "mozilla/layers/ImageDataSerializer.h"
#include "mozilla/layers/LayersSurfaces.h"
#include "mozilla/layers/TextureSourceProvider.h"
#include "mozilla/webrender/RenderThread.h"
#include "mozilla/webrender/WebRenderAPI.h"



namespace mozilla::layers {

class ScheduleHandleRenderTextureOps : public wr::NotificationHandler {
 public:
  explicit ScheduleHandleRenderTextureOps() = default;

  virtual void Notify(wr::Checkpoint aCheckpoint) override {
    if (aCheckpoint == wr::Checkpoint::FrameTexturesUpdated) {
      MOZ_ASSERT(wr::RenderThread::IsInRenderThread());
      wr::RenderThread::Get()->HandleRenderTextureOps();
    } else {
      MOZ_ASSERT(aCheckpoint == wr::Checkpoint::TransactionDropped);
    }
  }

 protected:
};

WebRenderTextureHost::WebRenderTextureHost(
    TextureFlags aFlags, TextureHost* aTexture,
    const wr::ExternalImageId& aExternalImageId)
    : TextureHost(TextureHostType::Unknown, aFlags),
      mWrappedTextureHost(aTexture) {
  MOZ_ASSERT(mWrappedTextureHost);
  MOZ_ASSERT(!(aFlags & TextureFlags::DEALLOCATE_CLIENT));
  MOZ_COUNT_CTOR(WebRenderTextureHost);

  mExternalImageId = Some(aExternalImageId);
}

WebRenderTextureHost::~WebRenderTextureHost() {
  MOZ_COUNT_DTOR(WebRenderTextureHost);
}

wr::ExternalImageId WebRenderTextureHost::GetExternalImageKey() {
  if (IsValid()) {
    mWrappedTextureHost->EnsureRenderTexture(mExternalImageId);
  }
  MOZ_ASSERT(mWrappedTextureHost->mExternalImageId.isSome());
  return mWrappedTextureHost->mExternalImageId.ref();
}

bool WebRenderTextureHost::IsValid() { return mWrappedTextureHost->IsValid(); }

void WebRenderTextureHost::UnbindTextureSource() {
  if (mWrappedTextureHost->AsBufferTextureHost()) {
    mWrappedTextureHost->UnbindTextureSource();
  }
  TextureHost::UnbindTextureSource();
}

already_AddRefed<gfx::DataSourceSurface> WebRenderTextureHost::GetAsSurface(
    gfx::DataSourceSurface* aSurface) {
  return mWrappedTextureHost->GetAsSurface(aSurface);
}

gfx::ColorDepth WebRenderTextureHost::GetColorDepth() const {
  return mWrappedTextureHost->GetColorDepth();
}

gfx::YUVColorSpace WebRenderTextureHost::GetYUVColorSpace() const {
  return mWrappedTextureHost->GetYUVColorSpace();
}

gfx::ColorRange WebRenderTextureHost::GetColorRange() const {
  return mWrappedTextureHost->GetColorRange();
}

gfx::TransferFunction WebRenderTextureHost::GetTransferFunction() const {
  return mWrappedTextureHost->GetTransferFunction();
}

gfx::IntSize WebRenderTextureHost::GetSize() const {
  return mWrappedTextureHost->GetSize();
}

gfx::SurfaceFormat WebRenderTextureHost::GetFormat() const {
  return mWrappedTextureHost->GetFormat();
}

bool WebRenderTextureHost::NeedsYFlip() const {
  return mWrappedTextureHost->NeedsYFlip();
}

void WebRenderTextureHost::MaybeDestroyRenderTexture() {
  mExternalImageId = Nothing();
}

void WebRenderTextureHost::NotifyNotUsed() {
  mWrappedTextureHost->NotifyNotUsed();
  TextureHost::NotifyNotUsed();
}

void WebRenderTextureHost::MaybeNotifyForUse(wr::TransactionBuilder& aTxn) {
}

bool WebRenderTextureHost::IsWrappingSurfaceTextureHost() {
  return mWrappedTextureHost->IsWrappingSurfaceTextureHost();
}

void WebRenderTextureHost::PrepareForUse() {
  if ((IsWrappingSurfaceTextureHost() &&
       !mWrappedTextureHost->AsRemoteTextureHostWrapper()) ||
      mWrappedTextureHost->AsBufferTextureHost()) {
    wr::RenderThread::Get()->PrepareForUse(GetExternalImageKey());
  }
}

gfx::SurfaceFormat WebRenderTextureHost::GetReadFormat() const {
  return mWrappedTextureHost->GetReadFormat();
}

bool WebRenderTextureHost::NeedsDeferredDeletion() const {
  return mWrappedTextureHost->NeedsDeferredDeletion();
}

uint32_t WebRenderTextureHost::NumSubTextures() {
  return mWrappedTextureHost->NumSubTextures();
}

void WebRenderTextureHost::PushResourceUpdates(
    wr::TransactionBuilder& aResources, ResourceUpdateOp aOp,
    const Range<wr::ImageKey>& aImageKeys, const wr::ExternalImageId& aExtID) {
  MOZ_ASSERT(GetExternalImageKey() == aExtID);

  mWrappedTextureHost->PushResourceUpdates(aResources, aOp, aImageKeys, aExtID);
}

void WebRenderTextureHost::PushDisplayItems(
    wr::DisplayListBuilder& aBuilder, const wr::LayoutRect& aBounds,
    const wr::LayoutRect& aClip, wr::ImageRendering aFilter,
    const Range<wr::ImageKey>& aImageKeys, PushDisplayItemFlagSet aFlags) {
  MOZ_ASSERT(aImageKeys.length() > 0);

  mWrappedTextureHost->PushDisplayItems(aBuilder, aBounds, aClip, aFilter,
                                        aImageKeys, aFlags);
}

bool WebRenderTextureHost::SupportsExternalCompositing(
    WebRenderBackend aBackend) {
  return mWrappedTextureHost->SupportsExternalCompositing(aBackend);
}

void WebRenderTextureHost::SetReadFence(Fence* aReadFence) {
  return mWrappedTextureHost->SetReadFence(aReadFence);
}

AndroidHardwareBuffer* WebRenderTextureHost::GetAndroidHardwareBuffer() const {
  return mWrappedTextureHost->GetAndroidHardwareBuffer();
}

TextureHostType WebRenderTextureHost::GetTextureHostType() {
  return mWrappedTextureHost->GetTextureHostType();
}

}  
