/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_WIDGET_WINDOW_SURFACE_H
#define MOZILLA_WIDGET_WINDOW_SURFACE_H

#include "mozilla/gfx/2D.h"
#include "Units.h"

namespace mozilla {
namespace widget {

class WindowSurface {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WindowSurface);

  virtual already_AddRefed<gfx::DrawTarget> Lock(
      const LayoutDeviceIntRegion& aRegion) = 0;

  virtual void Commit(const LayoutDeviceIntRegion& aInvalidRegion) = 0;

  virtual bool IsFallback() const { return false; }

 protected:
  virtual ~WindowSurface() = default;
};

}  
}  

#endif  // MOZILLA_WIDGET_WINDOW_SURFACE_H
