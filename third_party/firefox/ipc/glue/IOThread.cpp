/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ipc/IOThread.h"
#include "mozilla/ipc/NodeController.h"
#include "mozilla/Preferences.h"


#  include "chrome/common/ipc_channel_posix.h"

namespace mozilla {
namespace ipc {


IOThread* IOThread::sSingleton = nullptr;

IOThread::IOThread(const char* aName) : base::Thread(aName) {
  sSingleton = this;
}

IOThread::~IOThread() {
  MOZ_ASSERT(!IsRunning());
  sSingleton = nullptr;
}

void IOThread::Startup() {
  if (XRE_IsParentProcess()) {
    auto* thread = new IOThreadParent();
    MOZ_RELEASE_ASSERT(thread == sSingleton);
  }
  MOZ_ASSERT(sSingleton);
}

void IOThread::Shutdown() {
  if (XRE_IsParentProcess()) {
    delete static_cast<IOThreadParent*>(sSingleton);
    MOZ_ASSERT(!sSingleton);
  }
}

void IOThread::StartThread() {
  if (!StartWithOptions(
          base::Thread::Options{MessageLoop::TYPE_IO,  0})) {
    MOZ_CRASH("Failed to create IPC I/O Thread");
  }
}

void IOThread::StopThread() {
  Stop();
}


IOThreadParent::IOThreadParent() : IOThread("IPC I/O Parent") {
  MOZ_RELEASE_ASSERT(XRE_IsParentProcess());

  mChannelKind = [] {
    return &IPC::ChannelPosix::sKind;
  }();

  StartThread();
}

IOThreadParent::~IOThreadParent() { StopThread(); }

void IOThreadParent::Init() {

  NodeController::InitBrokerProcess(mChannelKind);
}

void IOThreadParent::CleanUp() {
  NodeController::CleanUp();

}


IOThreadChild::IOThreadChild(IPC::Channel::ChannelHandle aClientHandle,
                             base::ProcessId aParentPid)
    : IOThread("IPC I/O Child"),
      mClientHandle(std::move(aClientHandle)),
      mParentPid(std::move(aParentPid)) {
  StartThread();
}

IOThreadChild::~IOThreadChild() { StopThread(); }

void IOThreadChild::Init() {
  mInitialPort =
      NodeController::InitChildProcess(std::move(mClientHandle), mParentPid);
}

void IOThreadChild::CleanUp() { NodeController::CleanUp(); }

}  
}  
