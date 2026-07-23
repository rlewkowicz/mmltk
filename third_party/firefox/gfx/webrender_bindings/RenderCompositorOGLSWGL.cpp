/* This Source Code Form is subject to the terms of the Mozilla Public
 *
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RenderCompositorOGLSWGL.h"
#include "mozilla/ScopeExit.h"

#include "GLContext.h"
#include "GLContextEGL.h"
#include "ScopedGLHelpers.h"
#include "mozilla/layers/BuildConstants.h"
#include "mozilla/layers/CompositorOGL.h"
#include "mozilla/layers/Effects.h"
#include "mozilla/layers/TextureHostOGL.h"
#include "mozilla/widget/CompositorWidget.h"
#include "OGLShaderProgram.h"


#if defined(MOZ_WIDGET_GTK)
#  include "mozilla/widget/GtkCompositorWidget.h"
#  include <gdk/gdk.h>
#endif

namespace mozilla {
using namespace layers;
using namespace gfx;
namespace wr {

extern LazyLogModule gRenderThreadLog;
#define LOG(...) MOZ_LOG(gRenderThreadLog, LogLevel::Debug, (__VA_ARGS__))

UniquePtr<RenderCompositor> RenderCompositorOGLSWGL::Create(
    const RefPtr<widget::CompositorWidget>& aWidget, nsACString& aError) {
  if (!aWidget->GetCompositorOptions().AllowSoftwareWebRenderOGL()) {
    return nullptr;
  }

  RefPtr<Compositor> compositor;
#if defined(MOZ_WIDGET_GTK)
  nsCString log;
  RefPtr<CompositorOGL> compositorOGL;
  compositorOGL = new CompositorOGL(aWidget);
  if (!compositorOGL->Initialize(&log)) {
    gfxCriticalNote << "Failed to initialize CompositorOGL for SWGL: "
                    << log.get();
    return nullptr;
  }
  compositor = compositorOGL;
#endif

  if (!compositor) {
    return nullptr;
  }

  void* ctx = wr_swgl_create_context();
  if (!ctx) {
    gfxCriticalNote << "Failed SWGL context creation for WebRender";
    return nullptr;
  }

  return MakeUnique<RenderCompositorOGLSWGL>(compositor, aWidget, ctx);
}

RenderCompositorOGLSWGL::RenderCompositorOGLSWGL(
    Compositor* aCompositor, const RefPtr<widget::CompositorWidget>& aWidget,
    void* aContext)
    : RenderCompositorLayersSWGL(aCompositor, aWidget, aContext) {
  LOG("RenderCompositorOGLSWGL::RenderCompositorOGLSWGL()");
}

RenderCompositorOGLSWGL::~RenderCompositorOGLSWGL() {
  LOG("RRenderCompositorOGLSWGL::~RenderCompositorOGLSWGL()");
}

gl::GLContext* RenderCompositorOGLSWGL::GetGLContext() {
  return mCompositor->AsCompositorOGL()->gl();
}

bool RenderCompositorOGLSWGL::MakeCurrent() {
  GetGLContext()->MakeCurrent();
  RenderCompositorLayersSWGL::MakeCurrent();
  return true;
}

EGLSurface RenderCompositorOGLSWGL::CreateEGLSurface() {
  MOZ_ASSERT(GetGLContext()->GetContextType() == gl::GLContextType::EGL);

  EGLSurface surface = EGL_NO_SURFACE;
  surface = gl::GLContextEGL::CreateEGLSurfaceForCompositorWidget(
      mWidget, gl::GLContextEGL::Cast(GetGLContext())->mSurfaceConfig);
  if (surface == EGL_NO_SURFACE) {
    const auto* renderThread = RenderThread::Get();
    gfxCriticalNote << "Failed to create EGLSurface. "
                    << renderThread->RendererCount() << " renderers, "
                    << renderThread->ActiveRendererCount() << " active.";
  }

  mFullRender = true;

  return surface;
}

void RenderCompositorOGLSWGL::DestroyEGLSurface() {
  MOZ_ASSERT(GetGLContext()->GetContextType() == gl::GLContextType::EGL);

  const auto& gle = gl::GLContextEGL::Cast(GetGLContext());
  const auto& egl = gle->mEgl;

  if (mEGLSurface) {
    gle->SetEGLSurfaceOverride(EGL_NO_SURFACE);
    gl::GLContextEGL::DestroySurface(*egl, mEGLSurface);
    mEGLSurface = EGL_NO_SURFACE;
  }
}

bool RenderCompositorOGLSWGL::BeginFrame() {
  MOZ_ASSERT(!mInFrame);
  RenderCompositorLayersSWGL::BeginFrame();


  return true;
}

RenderedFrameId RenderCompositorOGLSWGL::EndFrame(
    const nsTArray<DeviceIntRect>& aDirtyRects) {
  mFullRender = false;

  return RenderCompositorLayersSWGL::EndFrame(aDirtyRects);
}

void RenderCompositorOGLSWGL::HandleExternalImage(
    RenderTextureHost* aExternalImage, FrameSurface& aFrameSurface) {
  MOZ_ASSERT(aExternalImage);

  RefPtr<TextureSource> layer =
      aExternalImage->CreateTextureSource(mCompositor);
  if (layer) {
    RefPtr<TexturedEffect> texturedEffect = CreateTexturedEffect(
        aExternalImage->GetFormat(), layer, aFrameSurface.mFilter,
         true);

    auto size = layer->GetSize();
    gfx::Rect drawRect(0.0, 0.0, float(size.width), float(size.height));

    EffectChain effect;
    effect.mPrimaryEffect = texturedEffect;
    mCompositor->DrawQuad(drawRect, aFrameSurface.mClipRect, effect, 1.0,
                          aFrameSurface.mTransform, drawRect);
  }
}

void RenderCompositorOGLSWGL::GetCompositorCapabilities(
    CompositorCapabilities* aCaps) {
  RenderCompositor::GetCompositorCapabilities(aCaps);

  aCaps->max_update_rects = 0;
}

bool RenderCompositorOGLSWGL::RequestFullRender() { return mFullRender; }

void RenderCompositorOGLSWGL::Pause() {
#if defined(MOZ_WIDGET_GTK)
  mCompositor->Pause();
#endif
}

bool RenderCompositorOGLSWGL::Resume() {
#if defined(MOZ_WIDGET_GTK)
  bool resumed = mCompositor->Resume();
  if (!resumed) {
    RenderThread::Get()->HandleWebRenderError(WebRenderError::NEW_SURFACE);
    return false;
  }
#endif
  return true;
}

bool RenderCompositorOGLSWGL::IsPaused() {
  return false;
}

LayoutDeviceIntSize RenderCompositorOGLSWGL::GetBufferSize() {
  return mWidget->GetClientSize();
}

UniquePtr<RenderCompositorLayersSWGL::Tile>
RenderCompositorOGLSWGL::DoCreateTile(Surface* aSurface) {
  auto source = MakeRefPtr<TextureImageTextureSourceOGL>(
      mCompositor->AsCompositorOGL(), layers::TextureFlags::NO_FLAGS);

  return MakeUnique<TileOGL>(std::move(source), aSurface->TileSize());
}

bool RenderCompositorOGLSWGL::MaybeReadback(
    const gfx::IntSize& aReadbackSize, const wr::ImageFormat& aReadbackFormat,
    const Range<uint8_t>& aReadbackBuffer, bool* aNeedsYFlip) {
  MOZ_ASSERT(aReadbackFormat == wr::ImageFormat::BGRA8);
  const GLenum format = LOCAL_GL_BGRA;

  GetGLContext()->fReadPixels(0, 0, aReadbackSize.width, aReadbackSize.height,
                              format, LOCAL_GL_UNSIGNED_BYTE,
                              &aReadbackBuffer[0]);

  if (aNeedsYFlip) {
    *aNeedsYFlip = true;
  }

  return true;
}


class PBOUnpackSurface : public gfx::DataSourceSurface {
 public:
  MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(PBOUnpackSurface, override)

  explicit PBOUnpackSurface(const gfx::IntSize& aSize) : mSize(aSize) {}

  uint8_t* GetData() override { return nullptr; }
  int32_t Stride() override { return mSize.width * sizeof(uint32_t); }
  gfx::SurfaceType GetType() const override {
    return gfx::SurfaceType::DATA_ALIGNED;
  }
  gfx::IntSize GetSize() const override { return mSize; }
  gfx::SurfaceFormat GetFormat() const override {
    return gfx::SurfaceFormat::B8G8R8A8;
  }

  bool Map(MapType, MappedSurface* aMappedSurface) override {
    aMappedSurface->mData = GetData();
    aMappedSurface->mStride = Stride();
    return true;
  }

  void Unmap() override {}

 private:
  gfx::IntSize mSize;
};

RenderCompositorOGLSWGL::TileOGL::TileOGL(
    RefPtr<layers::TextureImageTextureSourceOGL>&& aTexture,
    const gfx::IntSize& aSize)
    : mTexture(aTexture) {
  auto* gl = mTexture->gl();
  if (gl && gl->HasPBOState() && gl->MakeCurrent()) {
    mSurface = new PBOUnpackSurface(aSize);
    gl->fGenBuffers(1, &mPBO);
    gl->fBindBuffer(LOCAL_GL_PIXEL_UNPACK_BUFFER, mPBO);
    gl->fBufferData(LOCAL_GL_PIXEL_UNPACK_BUFFER,
                    mSurface->Stride() * aSize.height, nullptr,
                    LOCAL_GL_DYNAMIC_DRAW);
    gl->fBindBuffer(LOCAL_GL_PIXEL_UNPACK_BUFFER, 0);
  } else {
    mSurface = gfx::Factory::CreateDataSourceSurface(
        aSize, gfx::SurfaceFormat::B8G8R8A8);
  }
}

RenderCompositorOGLSWGL::TileOGL::~TileOGL() {
  if (mPBO) {
    auto* gl = mTexture->gl();
    if (gl && gl->MakeCurrent()) {
      gl->fDeleteBuffers(1, &mPBO);
      mPBO = 0;
    }
  }
}

layers::DataTextureSource*
RenderCompositorOGLSWGL::TileOGL::GetTextureSource() {
  return mTexture.get();
}

bool RenderCompositorOGLSWGL::TileOGL::Map(wr::DeviceIntRect aDirtyRect,
                                           wr::DeviceIntRect aValidRect,
                                           void** aData, int32_t* aStride) {
  if (mPBO) {
    auto* gl = mTexture->gl();
    if (!gl) {
      return false;
    }
    gl->fBindBuffer(LOCAL_GL_PIXEL_UNPACK_BUFFER, mPBO);
    size_t stride = mSurface->Stride();
    size_t offset =
        stride * aValidRect.min.y + aValidRect.min.x * sizeof(uint32_t);
    size_t length = stride * (aValidRect.height() - 1) +
                    (aValidRect.width()) * sizeof(uint32_t);
    void* data = gl->fMapBufferRange(
        LOCAL_GL_PIXEL_UNPACK_BUFFER, offset, length,
        LOCAL_GL_MAP_WRITE_BIT | LOCAL_GL_MAP_INVALIDATE_BUFFER_BIT);
    gl->fBindBuffer(LOCAL_GL_PIXEL_UNPACK_BUFFER, 0);
    if (!data) {
      return false;
    }
    *aData = data;
    *aStride = stride;
  } else {
    gfx::DataSourceSurface::MappedSurface map;
    if (!mSurface->Map(gfx::DataSourceSurface::READ_WRITE, &map)) {
      return false;
    }
    MOZ_ASSERT(map.mData != nullptr);
    if (!mTexture->IsValid()) {
      *aData = map.mData + aValidRect.min.y * map.mStride +
               aValidRect.min.x * sizeof(uint32_t);

      *aStride = map.mStride;
      mSubSurface = nullptr;
    } else {
      *aData = map.mData;
      *aStride = aDirtyRect.width() * BytesPerPixel(mSurface->GetFormat());
      mSubSurface = Factory::CreateWrappingDataSourceSurface(
          (uint8_t*)*aData, *aStride,
          IntSize(aDirtyRect.width(), aDirtyRect.height()),
          mSurface->GetFormat());
    }
  }
  return true;
}

void RenderCompositorOGLSWGL::TileOGL::Unmap(const gfx::IntRect& aDirtyRect) {
  nsIntRegion dirty(aDirtyRect);
  if (mPBO) {
    auto* gl = mTexture->gl();
    if (!gl) {
      return;
    }
    gl->fBindBuffer(LOCAL_GL_PIXEL_UNPACK_BUFFER, mPBO);
    gl->fUnmapBuffer(LOCAL_GL_PIXEL_UNPACK_BUFFER);
    mTexture->Update(mSurface, &dirty);
    gl->fBindBuffer(LOCAL_GL_PIXEL_UNPACK_BUFFER, 0);
  } else {
    if (mSubSurface) {
      mSurface->Unmap();
      IntPoint srcOffset = {0, 0};
      IntPoint dstOffset = aDirtyRect.TopLeft();
      dirty.MoveBy(-dstOffset);
      mTexture->Update(mSubSurface, &dirty, &srcOffset, &dstOffset);
      mSubSurface = nullptr;
    } else {
      mSurface->Unmap();
      mTexture->Update(mSurface, &dirty);
    }
  }
}

}  
}  
