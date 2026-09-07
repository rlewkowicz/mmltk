/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(_include_gfx_ipc_GPUProcessImpl_h_)
#define _include_gfx_ipc_GPUProcessImpl_h_

#include "mozilla/ipc/ProcessChild.h"
#include "GPUParent.h"


namespace mozilla {
namespace gfx {

class GPUProcessImpl final : public ipc::ProcessChild {
 public:
  using ipc::ProcessChild::ProcessChild;
  virtual ~GPUProcessImpl();

  bool Init(int aArgc, char* aArgv[]) override;
  void CleanUp() override;

 private:
  RefPtr<GPUParent> mGPU = new GPUParent;

};

}  
}  

#endif
