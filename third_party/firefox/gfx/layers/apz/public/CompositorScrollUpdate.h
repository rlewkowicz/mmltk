/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_CompositorScrollUpdate_h
#define mozilla_layers_CompositorScrollUpdate_h

#include "Units.h"

namespace mozilla {
namespace layers {

struct CompositorScrollUpdate {
  struct Metrics {
    CSSPoint mVisualScrollOffset;
    CSSToParentLayerScale mZoom;

    bool operator==(const Metrics& aOther) const;
    bool operator!=(const Metrics& aOther) const { return !(*this == aOther); }
  };

  enum class Source {
    UserInteraction,
    Other
  };

  Metrics mMetrics;
  Source mSource;

  bool operator==(const CompositorScrollUpdate& aOther) const;
  bool operator!=(const CompositorScrollUpdate& aOther) const {
    return !(*this == aOther);
  }
};

}  
}  

#endif  // mozilla_layers_CompositorScrollUpdate_h
