/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_LAYERS_TEXTUREWRAPPINGIMAGE_H_
#define GFX_LAYERS_TEXTUREWRAPPINGIMAGE_H_

#include "mozilla/RefPtr.h"
#include "ImageContainer.h"
#include "mozilla/layers/TextureClient.h"

namespace mozilla {
namespace layers {

class TextureWrapperImage final : public Image {
 public:
  TextureWrapperImage(TextureClient* aClient, const gfx::IntRect& aPictureRect);
  virtual ~TextureWrapperImage();

  gfx::IntSize GetSize() const override;
  gfx::IntRect GetPictureRect() const override;
  already_AddRefed<gfx::SourceSurface> GetAsSourceSurface() override;
  TextureClient* GetTextureClient(KnowsCompositor* aKnowsCompositor) override;
  void OnPrepareForwardToHost() override;
  void OnAbandonForwardToHost() override;

 private:
  gfx::IntRect mPictureRect;
  RefPtr<TextureClient> mTextureClient;
};

}  
}  

#endif  // GFX_LAYERS_TEXTUREWRAPPINGIMAGE_H_
