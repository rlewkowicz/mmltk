/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_IMAGECLIENT_H
#define MOZILLA_GFX_IMAGECLIENT_H

#include <stdint.h>                             // for uint32_t, uint64_t
#include <sys/types.h>                          // for int32_t
#include "mozilla/RefPtr.h"                     // for RefPtr, already_AddRefed
#include "mozilla/gfx/Types.h"                  // for SurfaceFormat
#include "mozilla/layers/CompositableClient.h"  // for CompositableClient
#include "mozilla/layers/CompositorTypes.h"     // for CompositableType, etc
#include "mozilla/layers/LayersSurfaces.h"      // for SurfaceDescriptor
#include "ImageContainer.h"                     // for ClearImagesType
#include "mozilla/layers/TextureClient.h"       // for TextureClient, etc
#include "mozilla/mozalloc.h"                   // for operator delete
#include "nsCOMPtr.h"                           // for already_AddRefed
#include "nsRect.h"                             // for mozilla::gfx::IntRect

namespace mozilla {
namespace layers {

class CompositableForwarder;
class Image;
class ImageContainer;
class ImageClientSingle;

class ImageClient : public CompositableClient {
 public:
  static already_AddRefed<ImageClient> CreateImageClient(
      CompositableType aImageHostType, ImageUsageType aUsageType,
      CompositableForwarder* aFwd, TextureFlags aFlags);

  virtual ~ImageClient() = default;

  virtual bool UpdateImage(ImageContainer* aContainer) = 0;

  virtual void ClearImagesInHost(ClearImagesType aType) {}

  virtual void RemoveTexture(TextureClient* aTexture) override;

  virtual ImageClientSingle* AsImageClientSingle() { return nullptr; }

  static already_AddRefed<TextureClient> CreateTextureClientForImage(
      Image* aImage, KnowsCompositor* aForwarder);

  uint32_t GetLastUpdateGenerationCounter() {
    return mLastUpdateGenerationCounter;
  }

  virtual RefPtr<TextureClient> GetForwardedTexture() { return nullptr; }

  CompositableType mType;
  ImageUsageType mUsageType;

 protected:
  ImageClient(CompositableForwarder* aFwd, TextureFlags aFlags,
              CompositableType aType, ImageUsageType aUsageType);

  uint32_t mLastUpdateGenerationCounter;
};

class ImageClientSingle : public ImageClient {
 public:
  ImageClientSingle(CompositableForwarder* aFwd, TextureFlags aFlags,
                    CompositableType aType, ImageUsageType aUsageType);

  bool UpdateImage(ImageContainer* aContainer) override;

  void OnDetach() override;

  bool AddTextureClient(TextureClient* aTexture) override;

  TextureInfo GetTextureInfo() const override;

  void ClearImagesInHost(ClearImagesType aType) override;

  ImageClientSingle* AsImageClientSingle() override { return this; }

  RefPtr<TextureClient> GetForwardedTexture() override;

  bool IsEmpty() { return mBuffers.IsEmpty(); }

 protected:
  struct Buffer {
    RefPtr<TextureClient> mTextureClient;
    int32_t mImageSerial;
  };
  nsTArray<Buffer> mBuffers;
};

}  
}  

#endif
