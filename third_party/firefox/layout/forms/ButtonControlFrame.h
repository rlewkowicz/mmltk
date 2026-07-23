/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LAYOUT_FORMS_BUTTONCONTROLFRAME_H_
#define LAYOUT_FORMS_BUTTONCONTROLFRAME_H_

#include "nsBlockFrame.h"

class nsTextNode;

namespace mozilla {

class ButtonControlFrame : public nsBlockFrame {
 public:
  ButtonControlFrame(ComputedStyle* aStyle, nsPresContext* aPc,
                     ClassID aClassID)
      : nsBlockFrame(aStyle, aPc, aClassID) {
    MOZ_ASSERT(IsReplaced(), "Our subclasses should be replaced elements");
  }
  NS_DECL_ABSTRACT_FRAME(ButtonControlFrame)
  nsContainerFrame* GetContentInsertionFrame() override { return this; }
  nsresult HandleEvent(nsPresContext* aPresContext,
                       mozilla::WidgetGUIEvent* aEvent,
                       nsEventStatus* aEventStatus) override;

  void Reflow(nsPresContext*, ReflowOutput&, const ReflowInput&,
              nsReflowStatus&) override;

  static void EnsureNonEmptyLabel(nsAString&);
};

}  

#endif  // LAYOUT_FORMS_BUTTONCONTROLFRAME_H_
