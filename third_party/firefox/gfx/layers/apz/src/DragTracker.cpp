/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DragTracker.h"

#include "InputData.h"
#include "mozilla/Logging.h"

static mozilla::LazyLogModule sApzDrgLog("apz.drag");
#define DRAG_LOG(...) MOZ_LOG(sApzDrgLog, LogLevel::Debug, (__VA_ARGS__))

namespace mozilla {
namespace layers {

DragTracker::DragTracker() : mInDrag(false) {}

bool DragTracker::StartsDrag(const MouseInput& aInput) {
  return aInput.IsLeftButton() && aInput.mType == MouseInput::MOUSE_DOWN;
}

bool DragTracker::EndsDrag(const MouseInput& aInput) {
  return (aInput.IsLeftButton() && aInput.mType == MouseInput::MOUSE_UP) ||
         aInput.mType == MouseInput::MOUSE_DRAG_END;
}

void DragTracker::Update(const MouseInput& aInput) {
  if (StartsDrag(aInput)) {
    DRAG_LOG("Starting drag\n");
    mInDrag = true;
  } else if (EndsDrag(aInput)) {
    DRAG_LOG("Ending drag\n");
    mInDrag = false;
    mOnScrollbar = Nothing();
  }
}

bool DragTracker::InDrag() const { return mInDrag; }

bool DragTracker::IsOnScrollbar(bool aOnScrollbar) {
  if (!mOnScrollbar) {
    DRAG_LOG("Setting hitscrollbar %d\n", aOnScrollbar);
    mOnScrollbar = Some(aOnScrollbar);
  }
  return mOnScrollbar.value();
}

}  
}  
