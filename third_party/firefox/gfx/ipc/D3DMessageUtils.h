/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(_include_gfx_ipc_D3DMessageUtils_h_)
#define _include_gfx_ipc_D3DMessageUtils_h_

#include "chrome/common/ipc_message_utils.h"
#include "ipc/IPCMessageUtils.h"

typedef struct DXGI_ADAPTER_DESC DXGI_ADAPTER_DESC;

struct DxgiAdapterDesc {

  bool operator==(const DxgiAdapterDesc& aOther) const;
};

namespace IPC {

DEFINE_IPC_SERIALIZER_WITHOUT_FIELDS(DxgiAdapterDesc);

}  

#endif
