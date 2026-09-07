/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/MessageSender.h"

#include "mozilla/ErrorResult.h"
#include "mozilla/dom/MessageBroadcaster.h"

namespace mozilla::dom {

void MessageSender::InitWithCallback(ipc::MessageManagerCallback* aCallback) {
  if (mCallback) {
    return;
  }

  SetCallback(aCallback);

  if (mParentManager) {
    mParentManager->AddChildManager(this);
  }

  for (uint32_t i = 0; i < mPendingScripts.Length(); ++i) {
    LoadScript(mPendingScripts[i], false, mPendingScriptsGlobalStates[i],
               IgnoreErrors());
  }
}

}  
