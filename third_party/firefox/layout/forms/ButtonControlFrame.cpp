/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ButtonControlFrame.h"


using namespace mozilla;

namespace mozilla {

void ButtonControlFrame::EnsureNonEmptyLabel(nsAString& aLabel) {
  if (aLabel.IsEmpty()) {
    aLabel = u"\ufeff"_ns;
  }
}

nsresult ButtonControlFrame::HandleEvent(nsPresContext* aPresContext,
                                         WidgetGUIEvent* aEvent,
                                         nsEventStatus* aEventStatus) {
  if (IsContentDisabled()) {
    return nsBlockFrame::HandleEvent(aPresContext, aEvent, aEventStatus);
  }
  return NS_OK;
}

void ButtonControlFrame::Reflow(nsPresContext* aPc, ReflowOutput& aReflowOutput,
                                const ReflowInput& aReflowInput,
                                nsReflowStatus& aStatus) {
  nsBlockFrame::Reflow(aPc, aReflowOutput, aReflowInput, aStatus);
  aStatus.Reset();
}

}  
