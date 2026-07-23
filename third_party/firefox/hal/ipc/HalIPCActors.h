/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_HalIPCActors_h
#define mozilla_HalIPCActors_h

namespace mozilla {
namespace hal_ipc {

class PHalChild;
class PHalParent;

PHalChild* CreateHalChild();

PHalParent* CreateHalParent();

}  
}  

#endif  // mozilla_HalIPCActors_h
