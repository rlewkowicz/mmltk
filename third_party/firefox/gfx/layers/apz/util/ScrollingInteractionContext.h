/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_ScrollingInteractionContext_h
#define mozilla_layers_ScrollingInteractionContext_h

#include "mozilla/EventForwards.h"
#include "mozilla/layers/ScrollableLayerGuid.h"

namespace mozilla {
namespace layers {

class MOZ_STACK_CLASS ScrollingInteractionContext {
 private:
  static bool sScrollingToAnchor;

 public:
  static bool IsScrollingToAnchor();

  explicit ScrollingInteractionContext(bool aScrollingToAnchor);

  ~ScrollingInteractionContext();

 private:
  bool mOldScrollingToAnchor;
};

}  
}  

#endif /* mozilla_layers_ScrollingInteractionContext_h */
