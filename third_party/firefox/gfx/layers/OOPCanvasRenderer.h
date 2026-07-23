/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_LAYERS_OOPCANVASRENDERER_H_
#define MOZILLA_LAYERS_OOPCANVASRENDERER_H_

#include "nsISupportsImpl.h"

class nsICanvasRenderingContextInternal;

namespace mozilla {

namespace dom {
class HTMLCanvasElement;
}

namespace layers {
class CanvasClient;

class OOPCanvasRenderer final {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(OOPCanvasRenderer)

 public:
  explicit OOPCanvasRenderer(nsICanvasRenderingContextInternal* aContext)
      : mContext(aContext) {}

  dom::HTMLCanvasElement* mHTMLCanvasElement = nullptr;

  nsICanvasRenderingContextInternal* mContext = nullptr;

  CanvasClient* mCanvasClient = nullptr;

 private:
  ~OOPCanvasRenderer() = default;
};

}  
}  

#endif  // MOZILLA_LAYERS_OOPCANVASRENDERER_H_
