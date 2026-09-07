/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SyncObject.h"

#include "nsIXULRuntime.h"  // for BrowserTabsRemoteAutostart

namespace mozilla {
namespace layers {


already_AddRefed<SyncObjectClient>
SyncObjectClient::CreateSyncObjectClientForContentDevice(SyncHandle aHandle) {
  return nullptr;
}

}  
}  
