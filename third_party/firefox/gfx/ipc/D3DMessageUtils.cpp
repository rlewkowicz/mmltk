/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ipc/IPCMessageUtils.h"
#include "D3DMessageUtils.h"

bool DxgiAdapterDesc::operator==(const DxgiAdapterDesc& aOther) const {
  return memcmp(&aOther, this, sizeof(*this)) == 0;
}


namespace IPC {


}  
