/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_IMAGE_WEBRENDERIMAGEPROVIDER_H_
#define MOZILLA_IMAGE_WEBRENDERIMAGEPROVIDER_H_

#include "nsISupportsImpl.h"

namespace mozilla {
namespace layers {
class RenderRootStateManager;
}

namespace wr {
class IpcResourceUpdateQueue;
struct ExternalImageId;
struct ImageKey;
}  

namespace image {

class ImageResource;
using ImageProviderId = uint32_t;

class WebRenderImageProvider {
 public:
  NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING

  ImageProviderId GetProviderId() const { return mProviderId; }

  static ImageProviderId AllocateProviderId();

  virtual nsresult UpdateKey(layers::RenderRootStateManager* aManager,
                             wr::IpcResourceUpdateQueue& aResources,
                             wr::ImageKey& aKey) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  virtual void InvalidateSurface() {}

 protected:
  WebRenderImageProvider(const ImageResource* aImage);

 private:
  ImageProviderId mProviderId;
};

}  
}  

#endif /* MOZILLA_IMAGE_WEBRENDERIMAGEPROVIDER_H_ */
