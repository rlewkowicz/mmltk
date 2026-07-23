/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _include_mozilla_gfx_ipc_GPUProcessListener_h_
#define _include_mozilla_gfx_ipc_GPUProcessListener_h_

#include "nsISupportsImpl.h"

namespace mozilla {
namespace gfx {

class GPUProcessListener {
  NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING

 public:
  virtual ~GPUProcessListener() = default;

  virtual void OnCompositorDestroyBackgrounded() {}

  virtual void OnCompositorUnexpectedShutdown() {}

  virtual void OnCompositorDeviceReset() {}
};

}  
}  

#endif  // _include_mozilla_gfx_ipc_GPUProcessListener_h_
