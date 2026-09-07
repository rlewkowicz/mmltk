/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_PresShellInlines_h
#define mozilla_PresShellInlines_h

#include "mozilla/PresShell.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/Element.h"

namespace mozilla {

void PresShell::SetNeedLayoutFlush() {
  mNeedLayoutFlush = true;
  if (dom::Document* doc = mDocument->GetDisplayDocument()) {
    if (PresShell* presShell = doc->GetPresShell()) {
      presShell->mNeedLayoutFlush = true;
    }
  }
}

void PresShell::SetNeedStyleFlush() {
  mNeedStyleFlush = true;
  if (dom::Document* doc = mDocument->GetDisplayDocument()) {
    if (PresShell* presShell = doc->GetPresShell()) {
      presShell->mNeedStyleFlush = true;
    }
  }
}

void PresShell::EnsureStyleFlush() {
  SetNeedStyleFlush();
  ScheduleFlush();
}

void PresShell::EnsureLayoutFlush() {
  SetNeedLayoutFlush();
  ScheduleFlush();
}

void PresShell::SetNeedThrottledAnimationFlush() {
  mNeedThrottledAnimationFlush = true;
  if (dom::Document* doc = mDocument->GetDisplayDocument()) {
    if (PresShell* presShell = doc->GetPresShell()) {
      presShell->mNeedThrottledAnimationFlush = true;
    }
  }
}

ServoStyleSet* PresShell::StyleSet() const {
  return mDocument->StyleSetForPresShell();
}

inline void PresShell::EventHandler::OnPresShellDestroy(Document* aDocument) {
  if (sLastKeyDownEventTargetElement &&
      sLastKeyDownEventTargetElement->OwnerDoc() == aDocument) {
    sLastKeyDownEventTargetElement = nullptr;
  }
}

}  

#endif  // mozilla_PresShellInlines_h
