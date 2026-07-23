/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_OverscrollHandoffChain_h
#define mozilla_layers_OverscrollHandoffChain_h

#include <vector>
#include "mozilla/RefPtr.h"   // for RefPtr
#include "nsISupportsImpl.h"  // for NS_INLINE_DECL_THREADSAFE_REFCOUNTING
#include "APZUtils.h"         // for CancelAnimationFlags
#include "mozilla/layers/LayersTypes.h"  // for Layer::ScrollDirection
#include "Units.h"                       // for ScreenPoint

namespace mozilla {

class InputData;

namespace layers {

class AsyncPanZoomController;

class OverscrollHandoffChain {
 protected:
  ~OverscrollHandoffChain();

 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(OverscrollHandoffChain)

  void Add(AsyncPanZoomController* aApzc);
  void SortByScrollPriority();

  uint32_t Length() const { return mChain.size(); }
  const RefPtr<AsyncPanZoomController>& GetApzcAtIndex(uint32_t aIndex) const;
  uint32_t IndexOf(const AsyncPanZoomController* aApzc) const;


  void FlushRepaints() const;

  void CancelAnimations(CancelAnimationFlags aFlags = Default) const;

  void ClearOverscroll() const;

  void SnapBackOverscrolledApzc(const AsyncPanZoomController* aStart) const;

  void SnapBackOverscrolledApzcForMomentum(
      const AsyncPanZoomController* aStart,
      const ParentLayerPoint& aVelocity) const;

  bool CanBePanned(const AsyncPanZoomController* aApzc) const;

  bool CanScrollInDirection(const AsyncPanZoomController* aApzc,
                            ScrollDirection aDirection) const;

  bool HasOverscrolledApzc() const;

  bool HasFastFlungApzc() const;

  bool HasAutoscrollApzc() const;

  enum class IncludeOverscroll : bool { No, Yes };
  RefPtr<AsyncPanZoomController> FindFirstScrollable(
      const InputData& aInput, ScrollDirections* aOutAllowedScrollDirections,
      IncludeOverscroll aIncludeOverscroll = IncludeOverscroll::Yes) const;

  std::tuple<bool, const AsyncPanZoomController*>
  ScrollingDownWillMoveDynamicToolbar(
      const AsyncPanZoomController* aApzc) const;

  bool ScrollingUpWillTriggerPullToRefresh(
      const AsyncPanZoomController* aApzc) const;

 private:
  std::vector<RefPtr<AsyncPanZoomController>> mChain;

  typedef void (AsyncPanZoomController::*APZCMethod)();
  typedef bool (AsyncPanZoomController::*APZCPredicate)() const;
  void ForEachApzc(APZCMethod aMethod) const;
  bool AnyApzc(APZCPredicate aPredicate) const;
};

struct OverscrollHandoffState {
  OverscrollHandoffState(const OverscrollHandoffChain& aChain,
                         const ScreenPoint& aPanDistance,
                         ScrollSource aScrollSource)
      : mChain(aChain),
        mChainIndex(0),
        mPanDistance(aPanDistance),
        mScrollSource(aScrollSource) {}

  const OverscrollHandoffChain& mChain;

  uint32_t mChainIndex;

  const ScreenPoint mPanDistance;

  ScrollSource mScrollSource;

  ScreenPoint mTotalMovement;
};

struct FlingHandoffState {
  ParentLayerPoint mVelocity;

  RefPtr<const OverscrollHandoffChain> mChain;

  Maybe<TimeDuration> mTouchStartRestingTime;

  ParentLayerCoord mMinPanVelocity;

  bool mIsHandoff;

  RefPtr<const AsyncPanZoomController> mScrolledApzc;
};

}  
}  

#endif /* mozilla_layers_OverscrollHandoffChain_h */
