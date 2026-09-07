/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/MIDIManagerParent.h"

#include "mozilla/dom/MIDIPlatformService.h"

namespace mozilla::dom {

void MIDIManagerParent::ActorDestroy(ActorDestroyReason aWhy) {
  if (MIDIPlatformService::IsRunning()) {
    MIDIPlatformService::Get()->RemoveManager(this);
  }
}

mozilla::ipc::IPCResult MIDIManagerParent::RecvRefresh() {
  MIDIPlatformService::Get()->Refresh();
  return IPC_OK();
}

mozilla::ipc::IPCResult MIDIManagerParent::RecvShutdown() {
  Close();
  return IPC_OK();
}

}  
