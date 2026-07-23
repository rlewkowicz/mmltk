/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(dom_tabs_ContentThread_h)
#define dom_tabs_ContentThread_h 1

#include "ContentChild.h"
#include "mozilla/ipc/ProcessChild.h"
#include "nsXREDirProvider.h"


namespace mozilla::dom {

class ContentProcess : public mozilla::ipc::ProcessChild {
  using ProcessChild = mozilla::ipc::ProcessChild;

 public:
  ContentProcess(IPC::Channel::ChannelHandle aClientChannel,
                 ProcessId aParentPid, const nsID& aMessageChannelId);
  ~ContentProcess();

  virtual bool Init(int aArgc, char* aArgv[]) override;
  virtual void CleanUp() override;

 private:
  void InfallibleInit(int aArgc, char* aArgv[]);

  ContentChild mContent;
  nsXREDirProvider mDirProvider;
};

}  

#endif
