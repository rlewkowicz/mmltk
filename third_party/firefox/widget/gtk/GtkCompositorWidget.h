/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_gtk_GtkCompositorWidget_h
#define widget_gtk_GtkCompositorWidget_h

#include "GLDefs.h"
#include "mozilla/DataMutex.h"
#include "mozilla/widget/CompositorWidget.h"
#include "WindowSurfaceProvider.h"
#include "mozilla/UniquePtr.h"
#include "WaylandSurfaceLock.h"

class nsIWidget;
class nsWindow;

namespace mozilla {

namespace layers {
class NativeLayerRootWayland;
}  

namespace widget {

class PlatformCompositorWidgetDelegate : public CompositorWidgetDelegate {
 public:
  virtual void NotifyClientSizeChanged(
      const LayoutDeviceIntSize& aClientSize) = 0;
  virtual void NotifyFullscreenChanged(bool aIsFullscreen) = 0;
  virtual GtkCompositorWidget* AsGtkCompositorWidget() { return nullptr; };

  virtual void CleanupResources() = 0;


  PlatformCompositorWidgetDelegate* AsPlatformSpecificDelegate() override {
    return this;
  }
};

class GtkCompositorWidgetInitData;

class GtkCompositorWidget : public CompositorWidget,
                            public PlatformCompositorWidgetDelegate {
 public:
  GtkCompositorWidget(const GtkCompositorWidgetInitData& aInitData,
                      const layers::CompositorOptions& aOptions,
                      RefPtr<nsWindow> aWindow );
  ~GtkCompositorWidget();


  already_AddRefed<gfx::DrawTarget> StartRemoteDrawing() override;
  void EndRemoteDrawing() override;

  already_AddRefed<gfx::DrawTarget> StartRemoteDrawingInRegion(
      const LayoutDeviceIntRegion& aInvalidRegion) override;
  void EndRemoteDrawingInRegion(
      gfx::DrawTarget* aDrawTarget,
      const LayoutDeviceIntRegion& aInvalidRegion) override;

  LayoutDeviceIntSize GetClientSize() override;

  nsIWidget* RealWidget() override;
  GtkCompositorWidget* AsGTK() override { return this; }
  CompositorWidgetDelegate* AsDelegate() override { return this; }

  EGLNativeWindowType GetEGLNativeWindow();

  LayoutDeviceIntRegion GetTransparentRegion() override;

  void CleanupResources() override;

  void SetEGLNativeWindowSize(const LayoutDeviceIntSize& aEGLWindowSize);

#if defined(MOZ_WAYLAND)
  mozilla::layers::NativeLayerRoot* GetNativeLayerRoot() override;
#endif


  void NotifyClientSizeChanged(const LayoutDeviceIntSize& aClientSize) override;
  void NotifyFullscreenChanged(bool aIsFullscreen) override;

  UniquePtr<WaylandSurfaceLock> LockSurface();

 private:
#if defined(MOZ_WAYLAND)
  void ConfigureWaylandBackend();
#endif
#ifdef MOZ_LOGGING
  bool IsPopup();
#endif

 protected:
  RefPtr<nsWindow> mWidget;

 private:
  DataMutex<LayoutDeviceIntSize> mClientSize;

  WindowSurfaceProvider mProvider;

#ifdef MOZ_WAYLAND
  RefPtr<mozilla::layers::NativeLayerRootWayland> mNativeLayerRoot;
#endif
};

}  
}  

#endif  // widget_gtk_GtkCompositorWidget_h
