/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CANVASIMAGECACHE_H_
#define CANVASIMAGECACHE_H_

#include "mozilla/Maybe.h"
#include "mozilla/gfx/Rect.h"
#include "nsSize.h"

namespace mozilla {
namespace dom {
class Element;
class CanvasRenderingContext2D;
}  
namespace gfx {
class DrawTarget;
class SourceSurface;
}  
}  
class imgIContainer;

namespace mozilla {

class CanvasImageCache {
  using SourceSurface = mozilla::gfx::SourceSurface;

 public:
  static void NotifyDrawImage(dom::Element* aImage,
                              dom::CanvasRenderingContext2D* aContext,
                              gfx::DrawTarget* aTarget, SourceSurface* aSource,
                              const gfx::IntSize& aSize,
                              const gfx::IntSize& aIntrinsicSize,
                              const Maybe<gfx::IntRect>& aCropRect);

  static void NotifyCanvasDestroyed(dom::CanvasRenderingContext2D* aContext);

  static SourceSurface* LookupAllCanvas(dom::Element* aImage,
                                        gfx::DrawTarget* aTarget);

  static SourceSurface* LookupCanvas(dom::Element* aImage,
                                     dom::CanvasRenderingContext2D* aContext,
                                     gfx::DrawTarget* aTarget,
                                     gfx::IntSize* aSizeOut,
                                     gfx::IntSize* aIntrinsicSizeOut,
                                     Maybe<gfx::IntRect>* aCropRectOut);
};

}  

#endif /* CANVASIMAGECACHE_H_ */
