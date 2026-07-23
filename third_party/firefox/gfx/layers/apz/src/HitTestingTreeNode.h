/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_HitTestingTreeNode_h
#define mozilla_layers_HitTestingTreeNode_h

#include "mozilla/gfx/CompositorHitTestInfo.h"
#include "mozilla/gfx/Matrix.h"                  // for Matrix4x4
#include "mozilla/layers/LayersTypes.h"          // for EventRegions
#include "mozilla/layers/ScrollableLayerGuid.h"  // for ScrollableLayerGuid
#include "mozilla/layers/ScrollbarData.h"        // for ScrollbarData
#include "mozilla/Maybe.h"                       // for Maybe
#include "mozilla/RecursiveMutex.h"              // for RecursiveMutexAutoLock
#include "mozilla/RefPtr.h"                      // for nsRefPtr
namespace mozilla {
namespace layers {

class AsyncDragMetrics;
class AsyncPanZoomController;

class HitTestingTreeNode {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HitTestingTreeNode);

 private:
  ~HitTestingTreeNode();

 public:
  HitTestingTreeNode(AsyncPanZoomController* aApzc, bool aIsPrimaryHolder,
                     LayersId aLayersId);
  void RecycleWith(const RecursiveMutexAutoLock& aProofOfTreeLock,
                   AsyncPanZoomController* aApzc, LayersId aLayersId);
  void Destroy();

  bool IsRecyclable(const RecursiveMutexAutoLock& aProofOfTreeLock);


  void SetLastChild(HitTestingTreeNode* aChild);
  void SetPrevSibling(HitTestingTreeNode* aSibling);
  void MakeRoot();


  HitTestingTreeNode* GetFirstChild() const;
  HitTestingTreeNode* GetLastChild() const;
  HitTestingTreeNode* GetPrevSibling() const;
  HitTestingTreeNode* GetParent() const;

  bool IsAncestorOf(const HitTestingTreeNode* aOther) const;


  AsyncPanZoomController* GetApzc() const;
  AsyncPanZoomController* GetNearestContainingApzc() const;
  bool IsPrimaryHolder() const;
  LayersId GetLayersId() const;


  void SetHitTestData(
      const LayerIntRect& aVisibleRect, const LayerIntSize& aRemoteDocumentSize,
      const CSSTransformMatrix& aTransform,
      const EventRegionsOverride& aOverride,
      const Maybe<ScrollableLayerGuid::ViewID>& aAsyncZoomContainerId);


  void SetScrollbarData(const Maybe<uint64_t>& aScrollbarAnimationId,
                        const ScrollbarData& aScrollbarData);
  bool MatchesScrollDragMetrics(const AsyncDragMetrics& aDragMetrics,
                                LayersId aLayersId) const;
  bool IsScrollbarNode() const;  
  bool IsScrollbarContainerNode() const;  
  ScrollDirection GetScrollbarDirection() const;
  bool IsScrollThumbNode() const;  
  ScrollableLayerGuid::ViewID GetScrollTargetId() const;
  const ScrollbarData& GetScrollbarData() const;
  Maybe<uint64_t> GetScrollbarAnimationId() const;


  void SetFixedPosData(ScrollableLayerGuid::ViewID aFixedPosTarget,
                       SideBits aFixedPosSides,
                       const Maybe<uint64_t>& aFixedPositionAnimationId);
  ScrollableLayerGuid::ViewID GetFixedPosTarget() const;
  SideBits GetFixedPosSides() const;
  Maybe<uint64_t> GetFixedPositionAnimationId() const;

  void SetStickyPosData(ScrollableLayerGuid::ViewID aStickyPosTarget,
                        const LayerRectAbsolute& aScrollRangeOuter,
                        const LayerRectAbsolute& aScrollRangeInner,
                        const Maybe<uint64_t>& aStickyPositionAnimationId);
  ScrollableLayerGuid::ViewID GetStickyPosTarget() const;
  const LayerRectAbsolute& GetStickyScrollRangeOuter() const;
  const LayerRectAbsolute& GetStickyScrollRangeInner() const;
  Maybe<uint64_t> GetStickyPositionAnimationId() const;

  EventRegionsOverride GetEventRegionsOverride() const;
  const CSSTransformMatrix& GetTransform() const;
  LayerToScreenMatrix4x4 GetTransformToGecko(LayersId aRemoteLayersId) const;
  const LayerIntRect& GetVisibleRect() const;

  ScreenRect GetRemoteDocumentScreenRect(
      LayersId aRemoteDocumentLayersId) const;

  Maybe<ScrollableLayerGuid::ViewID> GetAsyncZoomContainerId() const;

  void Dump(const char* aPrefix = "") const;

 private:
  friend class HitTestingTreeNodeAutoLock;
  void Lock(const RecursiveMutexAutoLock& aProofOfTreeLock);
  void Unlock(const RecursiveMutexAutoLock& aProofOfTreeLock);

  void SetApzcParent(AsyncPanZoomController* aApzc);

  RefPtr<HitTestingTreeNode> mLastChild;
  RefPtr<HitTestingTreeNode> mPrevSibling;
  RefPtr<HitTestingTreeNode> mParent;

  RefPtr<AsyncPanZoomController> mApzc;
  bool mIsPrimaryApzcHolder;
  int mLockCount;

  LayersId mLayersId;

  Maybe<uint64_t> mScrollbarAnimationId;

  ScrollbarData mScrollbarData;

  Maybe<uint64_t> mFixedPositionAnimationId;

  ScrollableLayerGuid::ViewID mFixedPosTarget;
  SideBits mFixedPosSides = SideBits::eNone;

  ScrollableLayerGuid::ViewID mStickyPosTarget;
  LayerRectAbsolute mStickyScrollRangeOuter;
  LayerRectAbsolute mStickyScrollRangeInner;
  Maybe<uint64_t> mStickyPositionAnimationId;

  LayerIntRect mVisibleRect;

  LayerIntSize mRemoteDocumentSize;

  CSSTransformMatrix mTransform;

  Maybe<ScrollableLayerGuid::ViewID> mAsyncZoomContainerId;

  EventRegionsOverride mOverride;
};

class HitTestingTreeNodeAutoLock final {
 public:
  HitTestingTreeNodeAutoLock();
  ~HitTestingTreeNodeAutoLock();
  HitTestingTreeNodeAutoLock(HitTestingTreeNodeAutoLock&&) = default;
  HitTestingTreeNodeAutoLock& operator=(HitTestingTreeNodeAutoLock&&) = default;

  void Initialize(const RecursiveMutexAutoLock& aProofOfTreeLock,
                  already_AddRefed<HitTestingTreeNode> aNode,
                  RecursiveMutex& aTreeMutex);
  void Clear();

  explicit operator bool() const { return !!mNode; }
  bool operator!() const { return !mNode; }
  HitTestingTreeNode* operator->() const { return mNode.get(); }

  HitTestingTreeNode* Get(
      mozilla::RecursiveMutexAutoLock& aProofOfTreeLock) const {
    return mNode.get();
  }

 private:
  RefPtr<HitTestingTreeNode> mNode;
  RecursiveMutex* mTreeMutex;
};

}  
}  

#endif  // mozilla_layers_HitTestingTreeNode_h
