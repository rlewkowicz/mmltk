/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_BUFFERCLIENT_H
#define MOZILLA_GFX_BUFFERCLIENT_H

#include <stdint.h>  // for uint64_t

#include "mozilla/Assertions.h"  // for MOZ_CRASH
#include "mozilla/DataMutex.h"
#include "mozilla/RefPtr.h"     // for already_AddRefed, RefCounted
#include "mozilla/gfx/Types.h"  // for SurfaceFormat
#include "mozilla/layers/CompositorTypes.h"
#include "mozilla/layers/LayersTypes.h"    // for LayersBackend, TextureDumpMode
#include "mozilla/layers/TextureClient.h"  // for TextureClient
#include "mozilla/webrender/WebRenderTypes.h"  // for RenderRoot
#include "nsISupportsImpl.h"                   // for MOZ_COUNT_CTOR, etc

namespace mozilla {
namespace layers {

class CompositableClient;
class ImageBridgeChild;
class ImageContainer;
class CompositableForwarder;
class CompositableChild;
class TextureClientRecycleAllocator;
class ContentClientRemoteBuffer;

class CompositableClient {
 protected:
  virtual ~CompositableClient();

 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(CompositableClient)

  explicit CompositableClient(CompositableForwarder* aForwarder,
                              TextureFlags aFlags = TextureFlags::NO_FLAGS);

  virtual void Dump(std::stringstream& aStream, const char* aPrefix = "",
                    bool aDumpHtml = false,
                    TextureDumpMode aCompress = TextureDumpMode::Compress) {};

  virtual TextureInfo GetTextureInfo() const = 0;

  LayersBackend GetCompositorBackendType() const;

  already_AddRefed<TextureClient> CreateBufferTextureClient(
      gfx::SurfaceFormat aFormat, gfx::IntSize aSize,
      gfx::BackendType aMoz2dBackend = gfx::BackendType::NONE,
      TextureFlags aFlags = TextureFlags::DEFAULT);

  already_AddRefed<TextureClient> CreateTextureClientForDrawing(
      gfx::SurfaceFormat aFormat, gfx::IntSize aSize, BackendSelector aSelector,
      TextureFlags aTextureFlags,
      TextureAllocationFlags aAllocFlags = ALLOC_DEFAULT);

  virtual bool Connect(ImageContainer* aImageContainer = nullptr);

  void Destroy();

  bool IsConnected() const;

  CompositableForwarder* GetForwarder() const { return mForwarder; }

  CompositableHandle GetAsyncHandle() const;

  CompositableHandle GetIPCHandle() const { return mHandle; }

  virtual bool AddTextureClient(TextureClient* aClient);

  virtual void OnDetach() {}

  virtual void ClearCachedResources();

  virtual void HandleMemoryPressure();

  virtual void RemoveTexture(TextureClient* aTexture);

  void InitIPDL(const CompositableHandle& aHandle);

  TextureFlags GetTextureFlags() const { return mTextureFlags; }

  TextureClientRecycleAllocator* GetTextureClientRecycler();

  bool HasTextureClientRecycler() {
    auto lock = mTextureClientRecycler.Lock();
    return !!(*lock);
  }

  static void DumpTextureClient(std::stringstream& aStream,
                                TextureClient* aTexture,
                                TextureDumpMode aCompress);

 protected:
  RefPtr<CompositableForwarder> mForwarder;
  Atomic<TextureFlags> mTextureFlags;
  DataMutex<RefPtr<TextureClientRecycleAllocator>> mTextureClientRecycler;

  CompositableHandle mHandle;
  bool mIsAsync;

  friend class CompositableChild;
};

struct AutoRemoveTexture {
  explicit AutoRemoveTexture(CompositableClient* aCompositable,
                             TextureClient* aTexture = nullptr)
      : mTexture(aTexture), mCompositable(aCompositable) {}

  ~AutoRemoveTexture();

  RefPtr<TextureClient> mTexture;

 private:
  CompositableClient* mCompositable;
};

}  
}  

#endif
