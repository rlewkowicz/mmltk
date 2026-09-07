/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ScrollingInteractionContext.h"

namespace mozilla::layers {

bool ScrollingInteractionContext::sScrollingToAnchor = false;

bool ScrollingInteractionContext::IsScrollingToAnchor() {
  return sScrollingToAnchor;
}

ScrollingInteractionContext::ScrollingInteractionContext(
    bool aScrollingToAnchor)
    : mOldScrollingToAnchor(sScrollingToAnchor) {
  sScrollingToAnchor = aScrollingToAnchor;
}

ScrollingInteractionContext::~ScrollingInteractionContext() {
  sScrollingToAnchor = mOldScrollingToAnchor;
}

}  
