/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_ProcessChild_h
#define mozilla_ipc_ProcessChild_h

#include "Endpoint.h"
#include "base/message_loop.h"
#include "base/process.h"

#include "mozilla/GeckoArgs.h"
#include "mozilla/ipc/IOThread.h"


namespace mozilla {
namespace ipc {

class ProcessChild {
 protected:
  typedef base::ProcessId ProcessId;

 public:
  explicit ProcessChild(IPC::Channel::ChannelHandle aClientChannel,
                        ProcessId aParentPid, const nsID& aMessageChannelId);

  ProcessChild(const ProcessChild&) = delete;
  ProcessChild& operator=(const ProcessChild&) = delete;

  virtual ~ProcessChild();

  virtual bool Init(int aArgc, char* aArgv[]) = 0;

  static void AddPlatformBuildID(geckoargs::ChildProcessArgs& aExtraArgs);

  static bool InitPrefs(int aArgc, char* aArgv[]);

  virtual void CleanUp() {}

  static MessageLoop* message_loop() { return gProcessChild->mUILoop; }

  static void NotifiedImpendingShutdown();

  static bool ExpectingShutdown();

  static void QuickExit();

 protected:
  static ProcessChild* current() { return gProcessChild; }

  ProcessId ParentPid() { return mParentPid; }

  UntypedEndpoint TakeInitialEndpoint();

 private:
  static ProcessChild* gProcessChild;

  MessageLoop* mUILoop;
  ProcessId mParentPid;
  nsID mMessageChannelId;
  UniquePtr<IOThreadChild> mChildThread;
};

}  
}  

#endif  // ifndef mozilla_ipc_ProcessChild_h
