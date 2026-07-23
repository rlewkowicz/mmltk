/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_widget_CompositorWidget_h_)
#define mozilla_widget_CompositorWidget_h_

#include "nsISupports.h"
#include "mozilla/RefPtr.h"
#include "Units.h"
#include "mozilla/gfx/Rect.h"
#include "mozilla/layers/CompositorOptions.h"
#include "mozilla/layers/LayersTypes.h"

class nsIWidget;

namespace mozilla {
class VsyncObserver;
namespace gl {
class GLContext;
}  
namespace layers {
class Compositor;
class LayerManager;
class NativeLayerRoot;
}  
namespace gfx {
class DrawTarget;
class SourceSurface;
}  
namespace widget {

class WinCompositorWidget;
class GtkCompositorWidget;
class AndroidCompositorWidget;
class CocoaCompositorWidget;
class CompositorWidgetInitData;

class PlatformCompositorWidgetDelegate;

class CompositorWidgetDelegate {
 public:
  virtual PlatformCompositorWidgetDelegate* AsPlatformSpecificDelegate() {
    return nullptr;
  }
};

#if 0 || 0 || \
    defined(MOZ_WAYLAND) || 0
class CompositorWidgetParent;

class CompositorWidgetChild;

#  define MOZ_WIDGET_SUPPORTS_OOP_COMPOSITING
#endif

class WidgetRenderingContext {
 public:
};

class CompositorWidget {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(mozilla::widget::CompositorWidget)

  static RefPtr<CompositorWidget> CreateLocal(
      const CompositorWidgetInitData& aInitData,
      const layers::CompositorOptions& aOptions, nsIWidget* aWidget);

  virtual bool PreRender(WidgetRenderingContext* aContext) { return true; }

  virtual void PostRender(WidgetRenderingContext* aContext) {}

  virtual layers::NativeLayerRoot* GetNativeLayerRoot() { return nullptr; }

  virtual already_AddRefed<gfx::DrawTarget> StartRemoteDrawing();
  virtual already_AddRefed<gfx::DrawTarget> StartRemoteDrawingInRegion(
      const LayoutDeviceIntRegion& aInvalidRegion) {
    return StartRemoteDrawing();
  }

  virtual void EndRemoteDrawing() {}
  virtual void EndRemoteDrawingInRegion(
      gfx::DrawTarget* aDrawTarget,
      const LayoutDeviceIntRegion& aInvalidRegion) {
    EndRemoteDrawing();
  }

  virtual LayoutDeviceIntRegion GetTransparentRegion();

  virtual void CleanupWindowEffects() {}

  virtual bool InitCompositor(layers::Compositor* aCompositor) { return true; }

  virtual bool OnResumeComposition() { return true; }

  virtual LayoutDeviceIntSize GetClientSize() = 0;

  virtual uint32_t GetGLFrameBufferFormat();

  virtual nsIWidget* RealWidget() = 0;

  virtual void CleanupRemoteDrawing();

  virtual uintptr_t GetWidgetKey() { return 0; }

  virtual already_AddRefed<gfx::DrawTarget> GetBackBufferDrawTarget(
      gfx::DrawTarget* aScreenTarget, const gfx::IntRect& aRect,
      bool* aOutIsCleared);

  virtual already_AddRefed<gfx::SourceSurface> EndBackBufferDrawing();

  virtual void ObserveVsync(VsyncObserver* aObserver) = 0;

  const layers::CompositorOptions& GetCompositorOptions() { return mOptions; }

  virtual bool IsHidden() const { return false; }

  virtual RefPtr<VsyncObserver> GetVsyncObserver() const;

  virtual WinCompositorWidget* AsWindows() { return nullptr; }
  virtual GtkCompositorWidget* AsGTK() { return nullptr; }
  virtual AndroidCompositorWidget* AsAndroid() { return nullptr; }
  virtual CocoaCompositorWidget* AsCocoa() { return nullptr; }

  virtual CompositorWidgetDelegate* AsDelegate() { return nullptr; }

 protected:
  explicit CompositorWidget(const layers::CompositorOptions& aOptions);
  virtual ~CompositorWidget();

  RefPtr<gfx::DrawTarget> mLastBackBuffer;

  layers::CompositorOptions mOptions;
};

}  
}  

#endif
