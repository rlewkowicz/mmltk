/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ScrollLinkedEffectDetector.h"

#include "mozilla/dom/Document.h"
#include "nsThreadUtils.h"

namespace mozilla {
namespace layers {

uint32_t ScrollLinkedEffectDetector::sDepth = 0;
bool ScrollLinkedEffectDetector::sFoundScrollLinkedEffect = false;

void ScrollLinkedEffectDetector::PositioningPropertyMutated() {
  MOZ_ASSERT(NS_IsMainThread());

  if (sDepth > 0) {
    sFoundScrollLinkedEffect = true;
  }
}

ScrollLinkedEffectDetector::ScrollLinkedEffectDetector(
    dom::Document* aDoc, const TimeStamp& aTimeStamp)
    : mDocument(aDoc), mTimeStamp(aTimeStamp) {
  MOZ_ASSERT(NS_IsMainThread());
  sDepth++;
}

ScrollLinkedEffectDetector::~ScrollLinkedEffectDetector() {
  sDepth--;
  if (sDepth == 0) {
    if (sFoundScrollLinkedEffect) {
      mDocument->ReportHasScrollLinkedEffect(mTimeStamp);
      sFoundScrollLinkedEffect = false;
    }
  }
}

}  
}  
