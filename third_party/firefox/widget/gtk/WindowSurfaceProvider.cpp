/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WindowSurfaceProvider.h"

#include "gfxPlatformGtk.h"
#include "GtkCompositorWidget.h"
#include "mozilla/gfx/Logging.h"
#include "mozilla/layers/LayersTypes.h"
#include "nsWindow.h"
#include "mozilla/ScopeExit.h"
#include "WidgetUtilsGtk.h"

#ifdef MOZ_WAYLAND
#  include "mozilla/StaticPrefs_widget.h"
#  include "WindowSurfaceCairo.h"
#  include "WindowSurfaceWaylandMultiBuffer.h"
#endif

#undef LOG
#ifdef MOZ_LOGGING
#  include "mozilla/Logging.h"
#  include "nsTArray.h"
#  include "Units.h"
extern mozilla::LazyLogModule gWidgetLog;
#  define LOG(args) MOZ_LOG(gWidgetLog, mozilla::LogLevel::Debug, args)
#else
#  define LOG(args)
#endif /* MOZ_LOGGING */

namespace mozilla {
namespace widget {

using namespace mozilla::layers;

WindowSurfaceProvider::WindowSurfaceProvider()
    : mWindowSurface(nullptr),
      mMutex("WindowSurfaceProvider"),
      mWindowSurfaceValid(false)
{
}

WindowSurfaceProvider::~WindowSurfaceProvider() {
#ifdef MOZ_WAYLAND
  MOZ_DIAGNOSTIC_ASSERT(!mWidget,
                        "nsWindow reference is still live, we're leaking it!");
#endif
}

#ifdef MOZ_WAYLAND
bool WindowSurfaceProvider::Initialize(RefPtr<nsWindow> aWidget) {
  mWindowSurfaceValid = false;
  mWidget = std::move(aWidget);
  return true;
}
bool WindowSurfaceProvider::Initialize(GtkCompositorWidget* aCompositorWidget) {
  mWindowSurfaceValid = false;
  mCompositorWidget = aCompositorWidget;
  mWidget = static_cast<nsWindow*>(aCompositorWidget->RealWidget());
  return true;
}
#endif

void WindowSurfaceProvider::CleanupResources() {
  MutexAutoLock lock(mMutex);
  mWindowSurfaceValid = false;
#ifdef MOZ_WAYLAND
  mWidget = nullptr;
#endif
}

RefPtr<WindowSurface> WindowSurfaceProvider::CreateWindowSurface() {
#ifdef MOZ_WAYLAND
  if (GdkIsWaylandDisplay()) {
    if (!mWidget) {
      return nullptr;
    }
    if (mWidget->IsDragPopup()) {
      return MakeRefPtr<WindowSurfaceCairo>(mWidget);
    }
    return MakeRefPtr<WindowSurfaceWaylandMB>(mWidget, mCompositorWidget);
  }
#endif
  MOZ_RELEASE_ASSERT(false);
}

MOZ_PUSH_IGNORE_THREAD_SAFETY

already_AddRefed<gfx::DrawTarget>
WindowSurfaceProvider::StartRemoteDrawingInRegion(
    const LayoutDeviceIntRegion& aInvalidRegion) {
  if (aInvalidRegion.IsEmpty()) {
    return nullptr;
  }

  mMutex.Lock();
  auto unlockMutex = MakeScopeExit([&] { mMutex.Unlock(); });

  if (!mWindowSurfaceValid) {
    mWindowSurface = nullptr;
    mWindowSurfaceValid = true;
  }

  if (!mWindowSurface) {
    mWindowSurface = CreateWindowSurface();
    if (!mWindowSurface) {
      return nullptr;
    }
  }

  RefPtr<gfx::DrawTarget> dt = mWindowSurface->Lock(aInvalidRegion);
  if (dt) {
    unlockMutex.release();
  }

  return dt.forget();
}

void WindowSurfaceProvider::EndRemoteDrawingInRegion(
    gfx::DrawTarget* aDrawTarget, const LayoutDeviceIntRegion& aInvalidRegion) {
  mMutex.AssertCurrentThreadOwns();
  auto unlockMutex = MakeScopeExit([&] { mMutex.Unlock(); });

  if (!mWindowSurface || !mWindowSurfaceValid) {
    return;
  }
  mWindowSurface->Commit(aInvalidRegion);
}

MOZ_POP_THREAD_SAFETY

}  
}  
