/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_SHAREDSURFACESCHILD_H
#define MOZILLA_GFX_SHAREDSURFACESCHILD_H

#include <stdint.h>                            // for uint32_t, uint64_t
#include "mozilla/Maybe.h"                     // for Maybe
#include "mozilla/RefPtr.h"                    // for already_AddRefed
#include "mozilla/StaticPtr.h"                 // for StaticRefPtr
#include "mozilla/gfx/UserData.h"              // for UserDataKey
#include "mozilla/layers/LayersSurfaces.h"     // for SurfaceDescriptor
#include "mozilla/webrender/WebRenderTypes.h"  // for wr::ImageKey
#include "nsTArray.h"                          // for AutoTArray
#include "nsThreadUtils.h"                     // for Runnable
#include "ImageTypes.h"                        // for ContainerProducerID

namespace mozilla {
namespace layers {
class AnimationImageKeyData;
}  
}  

template <>
struct nsTArray_RelocationStrategy<mozilla::layers::AnimationImageKeyData> {
  typedef nsTArray_RelocateUsingMoveConstructor<
      mozilla::layers::AnimationImageKeyData>
      Type;
};

namespace mozilla {
namespace gfx {
class SourceSurface;
class SourceSurfaceSharedData;
}  

namespace wr {
class IpcResourceUpdateQueue;
}  

namespace layers {

class CompositorManagerChild;
class RenderRootStateManager;

class SharedSurfacesChild {
 public:
  SharedSurfacesChild() = delete;
  ~SharedSurfacesChild() = delete;

  static void Share(gfx::SourceSurfaceSharedData* aSurface);

  static nsresult Share(gfx::SourceSurface* aSurface, wr::ExternalImageId& aId);

  static nsresult Share(gfx::SourceSurface* aSurface,
                        Maybe<SurfaceDescriptor>& aDesc);

  static nsresult Share(gfx::SourceSurfaceSharedData* aSurface,
                        RenderRootStateManager* aManager,
                        wr::IpcResourceUpdateQueue& aResources,
                        wr::ImageKey& aKey);

  static nsresult Share(gfx::SourceSurface* aSurface,
                        RenderRootStateManager* aManager,
                        wr::IpcResourceUpdateQueue& aResources,
                        wr::ImageKey& aKey);

  static Maybe<wr::ExternalImageId> GetExternalId(
      const gfx::SourceSurfaceSharedData* aSurface);

  static gfx::SourceSurfaceSharedData* AsSourceSurfaceSharedData(
      gfx::SourceSurface* aSurface);

  class ImageKeyData {
   public:
    ImageKeyData(RenderRootStateManager* aManager,
                 const wr::ImageKey& aImageKey);
    virtual ~ImageKeyData();

    ImageKeyData(ImageKeyData&& aOther);
    ImageKeyData& operator=(ImageKeyData&& aOther);
    ImageKeyData(const ImageKeyData&) = delete;
    ImageKeyData& operator=(const ImageKeyData&) = delete;

    void MergeDirtyRect(const Maybe<gfx::IntRect>& aDirtyRect);

    Maybe<gfx::IntRect> TakeDirtyRect() { return std::move(mDirtyRect); }

    RefPtr<RenderRootStateManager> mManager;
    Maybe<gfx::IntRect> mDirtyRect;
    wr::ImageKey mImageKey;
  };

 private:
  friend class SharedSurfacesAnimation;

  class SharedUserData final : public Runnable {
   public:
    SharedUserData();
    virtual ~SharedUserData();

    SharedUserData(const SharedUserData& aOther) = delete;
    SharedUserData& operator=(const SharedUserData& aOther) = delete;

    SharedUserData(SharedUserData&& aOther) = delete;
    SharedUserData& operator=(SharedUserData&& aOther) = delete;

    static void Destroy(void* aClosure);

    NS_IMETHOD Run() override;

    const wr::ExternalImageId& Id() const { return mId; }

    void ClearShared() {
      mKeys.Clear();
      mShared = false;
    }

    bool IsShared() const { return mShared; }

    void MarkShared(const wr::ExternalImageId& aId) {
      MOZ_ASSERT(!mShared);
      mId = aId;
      mShared = true;
    }

    wr::ImageKey UpdateKey(RenderRootStateManager* aManager,
                           wr::IpcResourceUpdateQueue& aResources,
                           const Maybe<gfx::IntRect>& aDirtyRect);

   protected:
    AutoTArray<ImageKeyData, 1> mKeys;
    wr::ExternalImageId mId;
    bool mShared : 1;
  };

  static nsresult ShareInternal(gfx::SourceSurfaceSharedData* aSurface,
                                SharedUserData** aUserData);

  static void Unshare(const wr::ExternalImageId& aId, bool aReleaseId,
                      nsTArray<ImageKeyData>& aKeys);

  static void DestroySharedUserData(void* aClosure);

  static gfx::UserDataKey sSharedKey;
};

class AnimationImageKeyData final : public SharedSurfacesChild::ImageKeyData {
 public:
  AnimationImageKeyData(RenderRootStateManager* aManager,
                        const wr::ImageKey& aImageKey);

  virtual ~AnimationImageKeyData();

  AnimationImageKeyData(AnimationImageKeyData&& aOther);
  AnimationImageKeyData& operator=(AnimationImageKeyData&& aOther);

  AutoTArray<RefPtr<gfx::SourceSurfaceSharedData>, 2> mPendingRelease;
};

class SharedSurfacesAnimation final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(SharedSurfacesAnimation)

  SharedSurfacesAnimation() = default;

  void Destroy();

  nsresult SetCurrentFrame(gfx::SourceSurfaceSharedData* aSurface,
                           const gfx::IntRect& aDirtyRect);

  nsresult UpdateKey(gfx::SourceSurfaceSharedData* aSurface,
                     RenderRootStateManager* aManager,
                     wr::IpcResourceUpdateQueue& aResources,
                     wr::ImageKey& aKey);

  void ReleasePreviousFrame(RenderRootStateManager* aManager,
                            const wr::ExternalImageId& aId);

  void Invalidate(RenderRootStateManager* aManager);

 private:
  ~SharedSurfacesAnimation();

  void HoldSurfaceForRecycling(AnimationImageKeyData& aEntry,
                               gfx::SourceSurfaceSharedData* aSurface);

  AutoTArray<AnimationImageKeyData, 1> mKeys;
  wr::ExternalImageId mId;
};

}  
}  

#endif
