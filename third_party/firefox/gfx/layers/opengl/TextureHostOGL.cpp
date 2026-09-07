/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TextureHostOGL.h"

#include "GLContextEGL.h"  // for GLContext, etc
#include "GLLibraryEGL.h"  // for GLLibraryEGL
#include "GLUploadHelpers.h"
#include "GLReadTexImageHelper.h"
#include "gfx2DGlue.h"             // for ContentForFormat, etc
#include "mozilla/gfx/2D.h"        // for DataSourceSurface
#include "mozilla/gfx/BaseSize.h"  // for BaseSize
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/gfx/Logging.h"  // for gfxCriticalError
#include "mozilla/layers/Fence.h"
#include "mozilla/layers/ISurfaceAllocator.h"
#include "mozilla/webrender/RenderEGLImageTextureHost.h"
#include "mozilla/webrender/WebRenderAPI.h"
#include "nsRegion.h"             // for nsIntRegion
#include "GfxTexturesReporter.h"  // for GfxTexturesReporter


#if defined(MOZ_WIDGET_GTK)
#  include "mozilla/layers/DMABUFTextureHostOGL.h"
#endif

using namespace mozilla::gl;
using namespace mozilla::gfx;

namespace mozilla {
namespace layers {

class Compositor;

void ApplySamplingFilterToBoundTexture(gl::GLContext* aGL,
                                       gfx::SamplingFilter aSamplingFilter,
                                       GLuint aTarget) {
  GLenum filter =
      (aSamplingFilter == gfx::SamplingFilter::POINT ? LOCAL_GL_NEAREST
                                                     : LOCAL_GL_LINEAR);

  aGL->fTexParameteri(aTarget, LOCAL_GL_TEXTURE_MIN_FILTER, filter);
  aGL->fTexParameteri(aTarget, LOCAL_GL_TEXTURE_MAG_FILTER, filter);
}

already_AddRefed<TextureHost> CreateTextureHostOGL(
    const SurfaceDescriptor& aDesc, ISurfaceAllocator* aDeallocator,
    LayersBackend aBackend, TextureFlags aFlags) {
  RefPtr<TextureHost> result;
  switch (aDesc.type()) {

    case SurfaceDescriptor::TEGLImageDescriptor: {
      if (aDeallocator && !aDeallocator->IsSameProcess()) {
        gfxCriticalError()
            << "EGLImageDescriptor must only be used in same process";
        return nullptr;
      }
      const EGLImageDescriptor& desc = aDesc.get_EGLImageDescriptor();
      result = new EGLImageTextureHost(aFlags, (EGLImage)desc.image(),
                                       (EGLSync)desc.fence(), desc.size(),
                                       desc.hasAlpha());
      break;
    }

#if defined(MOZ_WIDGET_GTK)
    case SurfaceDescriptor::TSurfaceDescriptorDMABuf: {
      result = new DMABUFTextureHostOGL(aFlags, aDesc);
      if (!result->IsValid()) {
        gfxCriticalError() << "DMABuf surface import failed!";
        result = nullptr;
      }
      break;
    }
#endif


    case SurfaceDescriptor::TSurfaceDescriptorSharedGLTexture: {
      if (aDeallocator && !aDeallocator->IsSameProcess()) {
        gfxCriticalError() << "SurfaceDescriptorSharedGLTexture must only be "
                              "used in same process";
        return nullptr;
      }
      const auto& desc = aDesc.get_SurfaceDescriptorSharedGLTexture();
      result =
          new GLTextureHost(aFlags, desc.texture(), desc.target(),
                            (GLsync)desc.fence(), desc.size(), desc.hasAlpha());
      break;
    }
    default: {
      MOZ_ASSERT_UNREACHABLE("Unsupported SurfaceDescriptor type");
      break;
    }
  }
  return result.forget();
}

static gl::TextureImage::Flags FlagsToGLFlags(TextureFlags aFlags) {
  uint32_t result = TextureImage::NoFlags;

  if (aFlags & TextureFlags::USE_NEAREST_FILTER)
    result |= TextureImage::UseNearestFilter;
  if (aFlags & TextureFlags::ORIGIN_BOTTOM_LEFT)
    result |= TextureImage::OriginBottomLeft;
  if (aFlags & TextureFlags::DISALLOW_BIGIMAGE)
    result |= TextureImage::DisallowBigImage;

  return static_cast<gl::TextureImage::Flags>(result);
}

TextureImageTextureSourceOGL::TextureImageTextureSourceOGL(
    CompositorOGL* aCompositor, TextureFlags aFlags)
    : mGL(aCompositor->gl()),
      mCompositor(aCompositor),
      mFlags(aFlags),
      mIterating(false) {
  if (mCompositor) {
    mCompositor->RegisterTextureSource(this);
  }
}

TextureImageTextureSourceOGL::~TextureImageTextureSourceOGL() {
  DeallocateDeviceData();
}

void TextureImageTextureSourceOGL::DeallocateDeviceData() {
  mTexImage = nullptr;
  mGL = nullptr;
  if (mCompositor) {
    mCompositor->UnregisterTextureSource(this);
  }
  SetUpdateSerial(0);
}

bool TextureImageTextureSourceOGL::Update(gfx::DataSourceSurface* aSurface,
                                          nsIntRegion* aDestRegion,
                                          gfx::IntPoint* aSrcOffset,
                                          gfx::IntPoint* aDstOffset) {
  GLContext* gl = mGL;
  MOZ_ASSERT(gl);
  if (!gl || !gl->MakeCurrent()) {
    NS_WARNING(
        "trying to update TextureImageTextureSourceOGL without a GLContext");
    return false;
  }
  if (!aSurface) {
    gfxCriticalError() << "Invalid surface for OGL update";
    return false;
  }
  MOZ_ASSERT(aSurface);

  IntSize size = aSurface->GetSize();
  if (!mTexImage || (mTexImage->GetSize() != size && !aSrcOffset) ||
      mTexImage->GetContentType() !=
          gfx::ContentForFormat(aSurface->GetFormat())) {
    if (mFlags & TextureFlags::DISALLOW_BIGIMAGE) {
      GLint maxTextureSize;
      gl->fGetIntegerv(LOCAL_GL_MAX_TEXTURE_SIZE, &maxTextureSize);
      if (size.width > maxTextureSize || size.height > maxTextureSize) {
        NS_WARNING("Texture exceeds maximum texture size, refusing upload");
        return false;
      }
      mTexImage = CreateBasicTextureImage(
          gl, size, gfx::ContentForFormat(aSurface->GetFormat()),
          LOCAL_GL_CLAMP_TO_EDGE, FlagsToGLFlags(mFlags));
    } else {
      mTexImage = CreateTextureImage(
          gl, size, gfx::ContentForFormat(aSurface->GetFormat()),
          LOCAL_GL_CLAMP_TO_EDGE, FlagsToGLFlags(mFlags),
          SurfaceFormatToImageFormat(aSurface->GetFormat()));
    }
    ClearCachedFilter();

    if (aDestRegion && !aSrcOffset &&
        !aDestRegion->IsEqual(gfx::IntRect(0, 0, size.width, size.height))) {
      mTexImage->Resize(size);
    }
  }

  return mTexImage->UpdateFromDataSource(aSurface, aDestRegion, aSrcOffset,
                                         aDstOffset);
}

void TextureImageTextureSourceOGL::EnsureBuffer(const IntSize& aSize,
                                                gfxContentType aContentType) {
  if (!mTexImage || mTexImage->GetSize() != aSize ||
      mTexImage->GetContentType() != aContentType) {
    mTexImage =
        CreateTextureImage(mGL, aSize, aContentType, LOCAL_GL_CLAMP_TO_EDGE,
                           FlagsToGLFlags(mFlags));
  }
  mTexImage->Resize(aSize);
}

gfx::IntSize TextureImageTextureSourceOGL::GetSize() const {
  if (mTexImage) {
    if (mIterating) {
      return mTexImage->GetTileRect().Size();
    }
    return mTexImage->GetSize();
  }
  NS_WARNING("Trying to query the size of an empty TextureSource.");
  return gfx::IntSize(0, 0);
}

gfx::SurfaceFormat TextureImageTextureSourceOGL::GetFormat() const {
  if (mTexImage) {
    return mTexImage->GetTextureFormat();
  }
  NS_WARNING("Trying to query the format of an empty TextureSource.");
  return gfx::SurfaceFormat::UNKNOWN;
}

gfx::IntRect TextureImageTextureSourceOGL::GetTileRect() {
  return mTexImage->GetTileRect();
}

void TextureImageTextureSourceOGL::BindTexture(
    GLenum aTextureUnit, gfx::SamplingFilter aSamplingFilter) {
  MOZ_ASSERT(mTexImage,
             "Trying to bind a TextureSource that does not have an underlying "
             "GL texture.");
  mTexImage->BindTexture(aTextureUnit);
  SetSamplingFilter(mGL, aSamplingFilter);
}


GLTextureSource::GLTextureSource(TextureSourceProvider* aProvider,
                                 GLuint aTextureHandle, GLenum aTarget,
                                 gfx::IntSize aSize, gfx::SurfaceFormat aFormat)
    : GLTextureSource(aProvider->GetGLContext(), aTextureHandle, aTarget, aSize,
                      aFormat) {}

GLTextureSource::GLTextureSource(GLContext* aGL, GLuint aTextureHandle,
                                 GLenum aTarget, gfx::IntSize aSize,
                                 gfx::SurfaceFormat aFormat)
    : mGL(aGL),
      mTextureHandle(aTextureHandle),
      mTextureTarget(aTarget),
      mSize(aSize),
      mFormat(aFormat) {
  MOZ_COUNT_CTOR(GLTextureSource);
}

GLTextureSource::~GLTextureSource() {
  MOZ_COUNT_DTOR(GLTextureSource);
  DeleteTextureHandle();
}

void GLTextureSource::DeallocateDeviceData() { DeleteTextureHandle(); }

void GLTextureSource::DeleteTextureHandle() {
  GLContext* gl = this->gl();
  if (mTextureHandle != 0 && gl && gl->MakeCurrent()) {
    gl->fDeleteTextures(1, &mTextureHandle);
  }
  mTextureHandle = 0;
}

void GLTextureSource::BindTexture(GLenum aTextureUnit,
                                  gfx::SamplingFilter aSamplingFilter) {
  MOZ_ASSERT(mTextureHandle != 0);
  GLContext* gl = this->gl();
  if (!gl || !gl->MakeCurrent()) {
    return;
  }
  gl->fActiveTexture(aTextureUnit);
  gl->fBindTexture(mTextureTarget, mTextureHandle);
  ApplySamplingFilterToBoundTexture(gl, aSamplingFilter, mTextureTarget);
}

bool GLTextureSource::IsValid() const { return !!gl() && mTextureHandle != 0; }


DirectMapTextureSource::DirectMapTextureSource(gl::GLContext* aContext,
                                               gfx::DataSourceSurface* aSurface)
    : GLTextureSource(aContext, 0, LOCAL_GL_TEXTURE_RECTANGLE_ARB,
                      aSurface->GetSize(), aSurface->GetFormat()),
      mSync(nullptr) {
  MOZ_ASSERT(aSurface);

  UpdateInternal(aSurface, nullptr, nullptr, true);
}

DirectMapTextureSource::DirectMapTextureSource(TextureSourceProvider* aProvider,
                                               gfx::DataSourceSurface* aSurface)
    : DirectMapTextureSource(aProvider->GetGLContext(), aSurface) {}

DirectMapTextureSource::~DirectMapTextureSource() {
  if (!mSync || !gl() || !gl()->MakeCurrent() || gl()->IsDestroyed()) {
    return;
  }

  gl()->fDeleteSync(mSync);
  mSync = nullptr;
}

bool DirectMapTextureSource::Update(gfx::DataSourceSurface* aSurface,
                                    nsIntRegion* aDestRegion,
                                    gfx::IntPoint* aSrcOffset,
                                    gfx::IntPoint* aDstOffset) {
  MOZ_RELEASE_ASSERT(aDstOffset == nullptr);
  if (!aSurface) {
    return false;
  }

  return UpdateInternal(aSurface, aDestRegion, aSrcOffset, false);
}

void DirectMapTextureSource::MaybeFenceTexture() {
  if (!gl() || !gl()->MakeCurrent() || gl()->IsDestroyed()) {
    return;
  }

  if (mSync) {
    gl()->fDeleteSync(mSync);
  }
  mSync = gl()->fFenceSync(LOCAL_GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

bool DirectMapTextureSource::Sync(bool aBlocking) {
  if (!gl() || !gl()->MakeCurrent() || gl()->IsDestroyed()) {
    return true;
  }

  if (!mSync) {
    return false;
  }

  GLenum waitResult =
      gl()->fClientWaitSync(mSync, LOCAL_GL_SYNC_FLUSH_COMMANDS_BIT,
                            aBlocking ? LOCAL_GL_TIMEOUT_IGNORED : 0);
  return waitResult == LOCAL_GL_ALREADY_SIGNALED ||
         waitResult == LOCAL_GL_CONDITION_SATISFIED;
}

bool DirectMapTextureSource::UpdateInternal(gfx::DataSourceSurface* aSurface,
                                            nsIntRegion* aDestRegion,
                                            gfx::IntPoint* aSrcOffset,
                                            bool aInit) {
  if (!gl() || !gl()->MakeCurrent()) {
    return false;
  }

  MOZ_ASSERT(gl()->IsExtensionSupported(gl::GLContext::APPLE_texture_range));
  MOZ_ASSERT(gl()->IsExtensionSupported(gl::GLContext::APPLE_client_storage));

  if (aInit) {
    gl()->fGenTextures(1, &mTextureHandle);
    gl()->fBindTexture(LOCAL_GL_TEXTURE_RECTANGLE_ARB, mTextureHandle);

    gl()->fTexParameteri(LOCAL_GL_TEXTURE_RECTANGLE_ARB,
                         LOCAL_GL_TEXTURE_STORAGE_HINT_APPLE,
                         LOCAL_GL_STORAGE_CACHED_APPLE);
    gl()->fTextureRangeAPPLE(LOCAL_GL_TEXTURE_RECTANGLE_ARB,
                             aSurface->Stride() * aSurface->GetSize().height,
                             aSurface->GetData());

    gl()->fTexParameteri(LOCAL_GL_TEXTURE_RECTANGLE_ARB,
                         LOCAL_GL_TEXTURE_WRAP_S, LOCAL_GL_CLAMP_TO_EDGE);
    gl()->fTexParameteri(LOCAL_GL_TEXTURE_RECTANGLE_ARB,
                         LOCAL_GL_TEXTURE_WRAP_T, LOCAL_GL_CLAMP_TO_EDGE);
  }

  MOZ_ASSERT(mTextureHandle);

  gl()->fPixelStorei(LOCAL_GL_UNPACK_CLIENT_STORAGE_APPLE, LOCAL_GL_TRUE);

  nsIntRegion destRegion = aDestRegion
                               ? *aDestRegion
                               : IntRect(0, 0, aSurface->GetSize().width,
                                         aSurface->GetSize().height);
  gfx::IntPoint srcPoint = aSrcOffset ? *aSrcOffset : gfx::IntPoint(0, 0);
  mFormat = gl::UploadSurfaceToTexture(
      gl(), aSurface, destRegion, mTextureHandle, aSurface->GetSize(), nullptr,
      aInit, srcPoint, gfx::IntPoint(0, 0), LOCAL_GL_TEXTURE0,
      LOCAL_GL_TEXTURE_RECTANGLE_ARB);

  if (mSync) {
    gl()->fDeleteSync(mSync);
    mSync = nullptr;
  }

  gl()->fPixelStorei(LOCAL_GL_UNPACK_CLIENT_STORAGE_APPLE, LOCAL_GL_FALSE);
  return true;
}




EGLImageTextureSource::EGLImageTextureSource(TextureSourceProvider* aProvider,
                                             EGLImage aImage,
                                             gfx::SurfaceFormat aFormat,
                                             GLenum aTarget, GLenum aWrapMode,
                                             gfx::IntSize aSize)
    : mGL(aProvider->GetGLContext()),
      mCompositor(aProvider->AsCompositorOGL()),
      mImage(aImage),
      mFormat(aFormat),
      mTextureTarget(aTarget),
      mWrapMode(aWrapMode),
      mSize(aSize) {
  MOZ_ASSERT(mTextureTarget == LOCAL_GL_TEXTURE_2D ||
             mTextureTarget == LOCAL_GL_TEXTURE_EXTERNAL);
}

void EGLImageTextureSource::BindTexture(GLenum aTextureUnit,
                                        gfx::SamplingFilter aSamplingFilter) {
  GLContext* gl = this->gl();
  if (!gl || !gl->MakeCurrent()) {
    NS_WARNING("Trying to bind a texture without a GLContext");
    return;
  }

#if defined(DEBUG)
  const bool supportsEglImage = [&]() {
    const auto& gle = GLContextEGL::Cast(gl);
    const auto& egl = gle->mEgl;

    return egl->HasKHRImageBase() &&
           egl->IsExtensionSupported(EGLExtension::KHR_gl_texture_2D_image) &&
           gl->IsExtensionSupported(GLContext::OES_EGL_image);
  }();
  MOZ_ASSERT(supportsEglImage, "EGLImage not supported or disabled in runtime");
#endif

  GLuint tex = mCompositor->GetTemporaryTexture(mTextureTarget, aTextureUnit);

  gl->fActiveTexture(aTextureUnit);
  gl->fBindTexture(mTextureTarget, tex);

  gl->fEGLImageTargetTexture2D(mTextureTarget, mImage);

  ApplySamplingFilterToBoundTexture(gl, aSamplingFilter, mTextureTarget);
}

bool EGLImageTextureSource::IsValid() const { return !!gl(); }

gfx::Matrix4x4 EGLImageTextureSource::GetTextureTransform() {
  gfx::Matrix4x4 ret;
  return ret;
}


EGLImageTextureHost::EGLImageTextureHost(TextureFlags aFlags, EGLImage aImage,
                                         EGLSync aSync, gfx::IntSize aSize,
                                         bool hasAlpha)
    : TextureHost(TextureHostType::EGLImage, aFlags),
      mImage(aImage),
      mSync(aSync),
      mSize(aSize),
      mHasAlpha(hasAlpha) {}

EGLImageTextureHost::~EGLImageTextureHost() = default;

gl::GLContext* EGLImageTextureHost::gl() const { return nullptr; }

gfx::SurfaceFormat EGLImageTextureHost::GetFormat() const {
  return mHasAlpha ? gfx::SurfaceFormat::R8G8B8A8
                   : gfx::SurfaceFormat::R8G8B8X8;
}

void EGLImageTextureHost::CreateRenderTexture(
    const wr::ExternalImageId& aExternalImageId) {
  MOZ_ASSERT(mExternalImageId.isSome());

  RefPtr texture = MakeRefPtr<wr::RenderEGLImageTextureHost>(
      mImage, mSync, mSize, GetFormat());
  wr::RenderThread::Get()->RegisterExternalImage(aExternalImageId,
                                                 texture.forget());
}

void EGLImageTextureHost::PushResourceUpdates(
    wr::TransactionBuilder& aResources, ResourceUpdateOp aOp,
    const Range<wr::ImageKey>& aImageKeys, const wr::ExternalImageId& aExtID) {
  auto method = aOp == TextureHost::ADD_IMAGE
                    ? &wr::TransactionBuilder::AddExternalImage
                    : &wr::TransactionBuilder::UpdateExternalImage;

  TextureHost::NativeTexturePolicy policy =
      TextureHost::BackendNativeTexturePolicy(
          aResources.GetCapabilities().mBackendType, GetSize());
  auto imageType = policy == TextureHost::NativeTexturePolicy::REQUIRE
                       ? wr::ExternalImageType::TextureHandle(
                             wr::ImageBufferKind::TextureRect)
                       : wr::ExternalImageType::TextureHandle(
                             wr::ImageBufferKind::TextureExternal);

  gfx::SurfaceFormat format = GetFormat();

  if (aImageKeys.length() != 1) {
    MOZ_ASSERT_UNREACHABLE("unexpected to be called");
    return;
  }

  wr::ImageDescriptor descriptor(GetSize(), wr::ImageFormat::BGRA8,
                                 format == gfx::SurfaceFormat::R8G8B8A8
                                     ? wr::OpacityType::HasAlphaChannel
                                     : wr::OpacityType::Opaque);
  (aResources.*method)(aImageKeys[0], descriptor, aExtID, imageType, 0,
                        false);
}

void EGLImageTextureHost::PushDisplayItems(
    wr::DisplayListBuilder& aBuilder, const wr::LayoutRect& aBounds,
    const wr::LayoutRect& aClip, wr::ImageRendering aFilter,
    const Range<wr::ImageKey>& aImageKeys, PushDisplayItemFlagSet aFlags) {
  bool preferCompositorSurface =
      aFlags.contains(PushDisplayItemFlag::PREFER_COMPOSITOR_SURFACE);
  bool supportsExternalCompositing =
      SupportsExternalCompositing(aBuilder.GetBackendType());

  if (aImageKeys.length() != 1) {
    MOZ_ASSERT_UNREACHABLE("unexpected key length");
    return;
  }
  aBuilder.PushImage(aBounds, aClip, true, false, aFilter, aImageKeys[0],
                     !(mFlags & TextureFlags::NON_PREMULTIPLIED),
                     wr::ColorF{1.0f, 1.0f, 1.0f, 1.0f},
                     preferCompositorSurface, supportsExternalCompositing);
}

bool EGLImageTextureHost::SupportsExternalCompositing(
    WebRenderBackend) {
  return false;
}


GLTextureHost::GLTextureHost(TextureFlags aFlags, GLuint aTextureHandle,
                             GLenum aTarget, GLsync aSync, gfx::IntSize aSize,
                             bool aHasAlpha)
    : TextureHost(TextureHostType::GLTexture, aFlags),
      mTexture(aTextureHandle),
      mTarget(aTarget),
      mSync(aSync),
      mSize(aSize),
      mHasAlpha(aHasAlpha) {}

GLTextureHost::~GLTextureHost() = default;

gl::GLContext* GLTextureHost::gl() const { return nullptr; }

gfx::SurfaceFormat GLTextureHost::GetFormat() const {
  MOZ_ASSERT(mTextureSource);
  return mTextureSource ? mTextureSource->GetFormat()
                        : gfx::SurfaceFormat::UNKNOWN;
}

}  
}  
