// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipc_channel.h"

#include "base/message_loop.h"
#include "mozilla/ipc/ProtocolUtils.h"

#  include "chrome/common/ipc_channel_posix.h"

namespace IPC {

Channel::Channel()
    : chan_cap_("ChannelImpl::SendMutex",
                MessageLoopForIO::current()->SerialEventTarget()) {}

Channel::~Channel() = default;

already_AddRefed<Channel> Channel::Create(ChannelHandle pipe, Mode mode,
                                          base::ProcessId other_pid) {
  if (auto* handle = std::get_if<mozilla::UniqueFileHandle>(&pipe)) {
    return mozilla::MakeAndAddRef<ChannelPosix>(std::move(*handle), mode,
                                                other_pid);
  }
  MOZ_ASSERT_UNREACHABLE("unhandled pipe type");
  return nullptr;
}

}  
