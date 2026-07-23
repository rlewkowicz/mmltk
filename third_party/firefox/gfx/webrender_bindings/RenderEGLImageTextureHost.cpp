/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RenderEGLImageTextureHost.h"

#include "mozilla/gfx/Logging.h"
#include "GLContextEGL.h"
#include "GLLibraryEGL.h"

namespace mozilla {
namespace wr {

RenderEGLImageTextureHost::RenderEGLImageTextureHost(EGLImage aImage,
                                                     EGLSync aSync,
                                                     gfx::IntSize aSize,
                                                     gfx::SurfaceFormat aFormat)
    : mImage(aImage),
      mSync(aSync),
      mSize(aSize),
      mFormat(aFormat),
      mTextureTarget(LOCAL_GL_TEXTURE_2D),
      mTextureHandle(0) {
  MOZ_COUNT_CTOR_INHERITED(RenderEGLImageTextureHost, RenderTextureHost);
}

RenderEGLImageTextureHost::~RenderEGLImageTextureHost() {
  MOZ_COUNT_DTOR_INHERITED(RenderEGLImageTextureHost, RenderTextureHost);
  DeleteTextureHandle();
}

wr::WrExternalImage RenderEGLImageTextureHost::Lock(uint8_t aChannelIndex,
                                                    gl::GLContext* aGL) {
  MOZ_ASSERT(aChannelIndex == 0);

  if (mGL.get() != aGL) {
    if (mGL) {
      MOZ_ASSERT_UNREACHABLE("Unexpected GL context");
      return InvalidToWrExternalImage();
    }
    mGL = aGL;
  }

  if (!mImage || !mGL || !mGL->MakeCurrent()) {
    return InvalidToWrExternalImage();
  }

  if (!WaitSync() || !CreateTextureHandle()) {
    return InvalidToWrExternalImage();
  }

  return NativeTextureToWrExternalImage(mTextureHandle, 0.0, 0.0,
                                        static_cast<float>(mSize.width),
                                        static_cast<float>(mSize.height));
}

void RenderEGLImageTextureHost::Unlock() {}

RefPtr<layers::TextureSource> RenderEGLImageTextureHost::CreateTextureSource(
    layers::TextureSourceProvider* aProvider) {
  gl::GLContext* gl = aProvider->GetGLContext();
  if (mGL.get() != gl) {
    if (mGL) {
      MOZ_ASSERT_UNREACHABLE("Unexpected GL context");
      return nullptr;
    }
    mGL = gl;
  }

  if (!WaitSync()) {
    return nullptr;
  }

  return new layers::EGLImageTextureSource(
      aProvider, mImage, mFormat, gl->GetPreferredEGLImageTextureTarget(),
      LOCAL_GL_CLAMP_TO_EDGE, mSize);
}

gfx::SurfaceFormat RenderEGLImageTextureHost::GetFormat() const {
  return mFormat;
}

bool RenderEGLImageTextureHost::CreateTextureHandle() {
  if (mTextureHandle) {
    return true;
  }

  mTextureTarget = mGL->GetPreferredEGLImageTextureTarget();
  MOZ_ASSERT(mTextureTarget == LOCAL_GL_TEXTURE_2D ||
             mTextureTarget == LOCAL_GL_TEXTURE_EXTERNAL);

  mGL->fGenTextures(1, &mTextureHandle);
  ActivateBindAndTexParameteri(mGL, LOCAL_GL_TEXTURE0, mTextureTarget,
                               mTextureHandle);
  mGL->fEGLImageTargetTexture2D(mTextureTarget, mImage);
  return true;
}

void RenderEGLImageTextureHost::DeleteTextureHandle() {
  if (mTextureHandle) {
    if (mGL && mGL->MakeCurrent()) {
      mGL->fDeleteTextures(1, &mTextureHandle);
    }
    mTextureHandle = 0;
  }
}

bool RenderEGLImageTextureHost::WaitSync() {
  bool syncSucceeded = true;
  if (mSync) {
    const auto& gle = gl::GLContextEGL::Cast(mGL);
    const auto& egl = gle->mEgl;
    MOZ_ASSERT(egl->IsExtensionSupported(gl::EGLExtension::KHR_fence_sync));
    if (egl->IsExtensionSupported(gl::EGLExtension::KHR_wait_sync)) {
      syncSucceeded = egl->fWaitSync(mSync, 0) == LOCAL_EGL_TRUE;
    } else {
      syncSucceeded = egl->fClientWaitSync(mSync, 0, LOCAL_EGL_FOREVER) ==
                      LOCAL_EGL_CONDITION_SATISFIED;
    }
    mSync = nullptr;
  }

  MOZ_ASSERT(
      syncSucceeded,
      "(Client)WaitSync generated an error. Has mSync already been destroyed?");
  return syncSucceeded;
}

}  
}  
