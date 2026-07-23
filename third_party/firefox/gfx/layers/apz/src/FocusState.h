/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_FocusState_h
#define mozilla_layers_FocusState_h

#include <unordered_map>  // for std::unordered_map
#include <unordered_set>  // for std::unordered_set

#include "mozilla/layers/FocusTarget.h"          // for FocusTarget
#include "mozilla/layers/ScrollableLayerGuid.h"  // for ViewID
#include "mozilla/Mutex.h"                       // for Mutex

namespace mozilla {
namespace layers {

class FocusState final {
 public:
  FocusState();

  uint64_t LastAPZProcessedEvent() const;

  void ReceiveFocusChangingEvent();

  void Update(LayersId aRootLayerTreeId, LayersId aOriginatingLayersId,
              const FocusTarget& aTarget);

  void RemoveFocusTarget(LayersId aLayersId);

  Maybe<ScrollableLayerGuid> GetHorizontalTarget() const;
  Maybe<ScrollableLayerGuid> GetVerticalTarget() const;

  bool CanIgnoreKeyboardShortcutMisses() const;

  void Reset();

 private:
  bool IsCurrent(const MutexAutoLock& aLock) const;

 private:
  mutable Mutex mMutex MOZ_UNANNOTATED;

  std::unordered_map<LayersId, FocusTarget, LayersId::HashFn> mFocusTree;

  uint64_t mLastAPZProcessedEvent;
  uint64_t mLastContentProcessedEvent;

  bool mFocusHasKeyEventListeners;
  bool mReceivedUpdate;

  LayersId mFocusLayersId;
  ScrollableLayerGuid::ViewID mFocusHorizontalTarget;
  ScrollableLayerGuid::ViewID mFocusVerticalTarget;
};

}  
}  

#endif  // mozilla_layers_FocusState_h
