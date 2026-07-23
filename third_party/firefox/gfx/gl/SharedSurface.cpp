/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SharedSurface.h"

#include "../2d/2D.h"
#include "GLBlitHelper.h"
#include "GLContext.h"
#include "GLReadTexImageHelper.h"
#include "GLScreenBuffer.h"
#include "nsThreadUtils.h"
#include "ScopedGLHelpers.h"
#include "SharedSurfaceGL.h"
#include "SharedSurfaceEGL.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/gfx/Logging.h"
#include "mozilla/layers/CompositorTypes.h"
#include "mozilla/layers/TextureClientSharedSurface.h"
#include "mozilla/layers/TextureForwarder.h"
#include "mozilla/StaticPrefs_webgl.h"



#if defined(MOZ_WIDGET_GTK)
#  include "gfxPlatformGtk.h"
#  include "SharedSurfaceDMABUF.h"
#  include "mozilla/widget/DMABufDevice.h"
#endif


namespace mozilla {
namespace gl {


SharedSurface::SharedSurface(const SharedSurfaceDesc& desc,
                             UniquePtr<MozFramebuffer> fb)
    : mDesc(desc), mFb(std::move(fb)) {}

SharedSurface::~SharedSurface() = default;

void SharedSurface::LockProd() {
  MOZ_ASSERT(!mIsLocked);

  LockProdImpl();

  mDesc.gl->LockSurface(this);
  mIsLocked = true;
}

void SharedSurface::UnlockProd() {
  if (!mIsLocked) return;

  UnlockProdImpl();

  mDesc.gl->UnlockSurface(this);
  mIsLocked = false;
}


UniquePtr<SurfaceFactory> SurfaceFactory::Create(
    GLContext* const pGl, const layers::TextureType consumerType) {
  auto& gl = *pGl;

  switch (consumerType) {
    case layers::TextureType::D3D11:
      break;

    case layers::TextureType::MacIOSurface:
      break;

    case layers::TextureType::DMABUF:
#if defined(MOZ_WIDGET_GTK)
      if (gl.GetContextType() == GLContextType::EGL &&
          widget::DMABufDevice::IsDMABufWebGLEnabled()) {
        return SurfaceFactory_DMABUF::Create(gl);
      }
#endif
      break;

    case layers::TextureType::AndroidNativeWindow:
      break;

    case layers::TextureType::AndroidHardwareBuffer:
      break;

    case layers::TextureType::EGLImage:
      break;

    case layers::TextureType::Unknown:
    case layers::TextureType::Last:
      break;
  }

  (void)gl;

  return nullptr;
}

SurfaceFactory::SurfaceFactory(const PartialSharedSurfaceDesc& partialDesc)
    : mDesc(partialDesc), mMutex("SurfaceFactor::mMutex") {}

SurfaceFactory::~SurfaceFactory() = default;

}  
}  
