/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_layers_SurfacePool_h)
#define mozilla_layers_SurfacePool_h

#include "GLTypes.h"
#include "nsISupportsImpl.h"
#include "nsRegion.h"

namespace mozilla {

namespace gl {
class GLContext;
}  

namespace layers {

class SurfacePoolHandle;

class SurfacePool {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(SurfacePool);

#if 0 || defined(MOZ_WAYLAND)
  static RefPtr<SurfacePool> Create(size_t aPoolSizeLimit);
#endif

  virtual RefPtr<SurfacePoolHandle> GetHandleForGL(gl::GLContext* aGL) = 0;
  virtual void DestroyGLResourcesForContext(gl::GLContext* aGL) = 0;

 protected:
  virtual ~SurfacePool() = default;
};

class SurfacePoolHandleCA;
class SurfacePoolHandleWayland;

class SurfacePoolHandle {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(SurfacePoolHandle);
  virtual SurfacePoolHandleCA* AsSurfacePoolHandleCA() { return nullptr; }
  virtual SurfacePoolHandleWayland* AsSurfacePoolHandleWayland() {
    return nullptr;
  }

  virtual RefPtr<SurfacePool> Pool() = 0;

  virtual void OnBeginFrame() = 0;
  virtual void OnEndFrame() = 0;

 protected:
  virtual ~SurfacePoolHandle() = default;
};

}  
}  

#endif
