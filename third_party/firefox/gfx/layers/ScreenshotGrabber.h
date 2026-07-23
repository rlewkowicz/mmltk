/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_ScreenshotGrabber_h
#define mozilla_layers_ScreenshotGrabber_h

#include "nsISupportsImpl.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Point.h"
#include "mozilla/gfx/Rect.h"

namespace mozilla {
namespace layers {

namespace frame_capture {
class Window;
class RenderSource;
class DownscaleTarget;
class AsyncReadbackBuffer;

}  

namespace frame_capture {

class Window {
 public:
  virtual already_AddRefed<RenderSource> GetWindowContents(
      const gfx::IntSize& aWindowSize) = 0;
  virtual already_AddRefed<DownscaleTarget> CreateDownscaleTarget(
      const gfx::IntSize& aSize) = 0;
  virtual already_AddRefed<AsyncReadbackBuffer> CreateAsyncReadbackBuffer(
      const gfx::IntSize& aSize) = 0;

 protected:
  virtual ~Window() = default;
};

class RenderSource {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(RenderSource)

  const auto& Size() const { return mSize; }

 protected:
  explicit RenderSource(const gfx::IntSize& aSize) : mSize(aSize) {}
  virtual ~RenderSource() = default;

  const gfx::IntSize mSize;
};

class DownscaleTarget {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(DownscaleTarget)

  virtual already_AddRefed<RenderSource> AsRenderSource() = 0;

  const auto& Size() const { return mSize; }
  virtual bool DownscaleFrom(RenderSource* aSource,
                             const gfx::IntRect& aSourceRect,
                             const gfx::IntRect& aDestRect) = 0;

 protected:
  explicit DownscaleTarget(const gfx::IntSize& aSize) : mSize(aSize) {}
  virtual ~DownscaleTarget() = default;

  const gfx::IntSize mSize;
};

class AsyncReadbackBuffer {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(
      mozilla::layers::frame_capture::AsyncReadbackBuffer)

  const auto& Size() const { return mSize; }
  virtual void CopyFrom(RenderSource* aSource) = 0;
  virtual bool MapAndCopyInto(gfx::DataSourceSurface* aSurface,
                              const gfx::IntSize& aReadSize) = 0;

 protected:
  explicit AsyncReadbackBuffer(const gfx::IntSize& aSize) : mSize(aSize) {}
  virtual ~AsyncReadbackBuffer() = default;

  const gfx::IntSize mSize;
};

}  

}  
}  

#endif  // mozilla_layers_ScreenshotGrabber_h
