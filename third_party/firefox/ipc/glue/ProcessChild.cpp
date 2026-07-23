/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ipc/ProcessChild.h"

#include "Endpoint.h"
#include "nsDebug.h"

#  include <unistd.h>  // for _exit()
#  include <time.h>
#  include "base/eintr_wrapper.h"
#  include "prenv.h"

#include "nsAppRunner.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/ipc/IOThread.h"
#include "mozilla/ipc/ProcessUtils.h"
#include "mozilla/GeckoArgs.h"

namespace mozilla {
namespace ipc {

ProcessChild* ProcessChild::gProcessChild;

ProcessChild::ProcessChild(IPC::Channel::ChannelHandle aClientChannel,
                           ProcessId aParentPid, const nsID& aMessageChannelId)
    : mUILoop(MessageLoop::current()),
      mParentPid(aParentPid),
      mMessageChannelId(aMessageChannelId),
      mChildThread(
          MakeUnique<IOThreadChild>(std::move(aClientChannel), aParentPid)) {
  MOZ_ASSERT(mUILoop, "UILoop should be created by now");
  MOZ_ASSERT(!gProcessChild, "should only be one ProcessChild");
  gProcessChild = this;
}

void ProcessChild::AddPlatformBuildID(geckoargs::ChildProcessArgs& aExtraArgs) {
  nsCString parentBuildID(mozilla::PlatformBuildID());
  geckoargs::sParentBuildID.Put(parentBuildID.get(), aExtraArgs);
}

bool ProcessChild::InitPrefs(int aArgc, char* aArgv[]) {
  Maybe<ReadOnlySharedMemoryHandle> prefsHandle =
      geckoargs::sPrefsHandle.Get(aArgc, aArgv);
  Maybe<ReadOnlySharedMemoryHandle> prefMapHandle =
      geckoargs::sPrefMapHandle.Get(aArgc, aArgv);

  if (prefsHandle.isNothing() || prefMapHandle.isNothing()) {
    return false;
  }

  SharedPreferenceDeserializer deserializer;
  return deserializer.DeserializeFromSharedMemory(std::move(*prefsHandle),
                                                  std::move(*prefMapHandle));
}

static void SleepIfEnv(const char* aName) {}

ProcessChild::~ProcessChild() {
#if defined(NS_FREE_PERMANENT_DATA)
  SleepIfEnv("MOZ_TEST_CHILD_EXIT_HANG");
#endif
  gProcessChild = nullptr;
}

void ProcessChild::NotifiedImpendingShutdown() {
  AppShutdown::SetImpendingShutdown();
}

void ProcessChild::QuickExit() {
#if !defined(NS_FREE_PERMANENT_DATA)
  SleepIfEnv("MOZ_TEST_CHILD_EXIT_HANG");
#endif
  AppShutdown::DoImmediateExit();
}

UntypedEndpoint ProcessChild::TakeInitialEndpoint() {
  return UntypedEndpoint{PrivateIPDLInterface{},
                         mChildThread->TakeInitialPort(), mMessageChannelId,
                         EndpointProcInfo::Current(),
                         EndpointProcInfo{.mPid = mParentPid, .mChildID = 0}};
}

}  
}  
