/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(_include_dom_media_ipc_RDDProcessImpl_h_)
#define _include_dom_media_ipc_RDDProcessImpl_h_
#include "mozilla/ipc/ProcessChild.h"


#include "RDDParent.h"

namespace mozilla {

class RDDProcessImpl final : public ipc::ProcessChild {
 public:
  using ipc::ProcessChild::ProcessChild;
  ~RDDProcessImpl();

  bool Init(int aArgc, char* aArgv[]) override;
  void CleanUp() override;

 private:
  RefPtr<RDDParent> mRDD = new RDDParent;

};

}  

#endif
