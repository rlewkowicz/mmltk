/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_WIDGET_GTK_WINDOW_SURFACE_PROVIDER_H
#define MOZILLA_WIDGET_GTK_WINDOW_SURFACE_PROVIDER_H

#include <gdk/gdk.h>

#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Types.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/widget/WindowSurface.h"
#include "Units.h"


class nsWindow;

namespace mozilla {
namespace widget {

class GtkCompositorWidget;

class WindowSurfaceProvider final {
 public:
  WindowSurfaceProvider();
  ~WindowSurfaceProvider();

#ifdef MOZ_WAYLAND
  bool Initialize(RefPtr<nsWindow> aWidget);
  bool Initialize(GtkCompositorWidget* aCompositorWidget);
#endif

  void CleanupResources();

  already_AddRefed<gfx::DrawTarget> StartRemoteDrawingInRegion(
      const LayoutDeviceIntRegion& aInvalidRegion);
  void EndRemoteDrawingInRegion(gfx::DrawTarget* aDrawTarget,
                                const LayoutDeviceIntRegion& aInvalidRegion);

 private:
  RefPtr<WindowSurface> CreateWindowSurface();
  void CleanupWindowSurface();

  RefPtr<WindowSurface> mWindowSurface;

  mozilla::Mutex mMutex MOZ_UNANNOTATED;
  bool mWindowSurfaceValid;
#ifdef MOZ_WAYLAND
  RefPtr<nsWindow> mWidget;
  GtkCompositorWidget* mCompositorWidget = nullptr;
#endif
};

}  
}  

#endif  // MOZILLA_WIDGET_GTK_WINDOW_SURFACE_PROVIDER_H
