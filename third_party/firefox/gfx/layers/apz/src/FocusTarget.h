/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_FocusTarget_h
#define mozilla_layers_FocusTarget_h

#include <stdint.h>  // for int32_t, uint32_t

#include "mozilla/DefineEnum.h"                  // for MOZ_DEFINE_ENUM
#include "mozilla/layers/ScrollableLayerGuid.h"  // for ViewID
#include "mozilla/Variant.h"                     // for Variant
#include "mozilla/Maybe.h"                       // for Maybe

namespace mozilla {

class PresShell;

namespace layers {

class FocusTarget final {
 public:
  struct ScrollTargets {
    ScrollableLayerGuid::ViewID mHorizontal;
    ScrollableLayerGuid::ViewID mVertical;

    bool operator==(const ScrollTargets& aRhs) const {
      return (mHorizontal == aRhs.mHorizontal && mVertical == aRhs.mVertical);
    }
  };

  struct NoFocusTarget {
    bool operator==(const NoFocusTarget& aRhs) const { return true; }
  };

  FocusTarget();

  FocusTarget(PresShell* aRootPresShell, uint64_t aFocusSequenceNumber);

  bool operator==(const FocusTarget& aRhs) const;

  const char* Type() const;

 public:
  uint64_t mSequenceNumber;

  bool mFocusHasKeyEventListeners;

  mozilla::Variant<LayersId, ScrollTargets, NoFocusTarget> mData;
};

}  
}  

#endif  // mozilla_layers_FocusTarget_h
