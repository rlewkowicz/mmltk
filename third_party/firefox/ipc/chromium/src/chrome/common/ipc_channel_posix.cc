// Copyright (c) 2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/ipc_channel_posix.h"
#include "mozilla/ScopeExit.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include "mozilla/Mutex.h"
#include <stddef.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/uio.h>

#include "base/command_line.h"
#include "base/eintr_wrapper.h"
#include "base/logging.h"
#include "base/process.h"
#include "base/process_util.h"
#include "base/string_util.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/ipc_message_utils.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/UniquePtr.h"

#if defined(IOV_MAX)
static const size_t kMaxIOVecSize = IOV_MAX;
#else
static const size_t kMaxIOVecSize = 16;
#endif

using namespace mozilla::ipc;

namespace IPC {

namespace {

bool ErrorIsBrokenPipe(int err) { return err == EPIPE || err == ECONNRESET; }


static inline ssize_t corrected_sendmsg(int socket,
                                        const struct msghdr* message,
                                        int flags) {
  return sendmsg(socket, message, flags);
}

}  

const Channel::ChannelKind ChannelPosix::sKind{
    .create_raw_pipe = &ChannelPosix::CreateRawPipe,
    .num_relayed_attachments = &ChannelPosix::NumRelayedAttachments,
    .is_valid_handle = &ChannelPosix::IsValidHandle,
};

ChannelPosix::ChannelPosix(mozilla::UniqueFileHandle pipe, Mode mode,
                           base::ProcessId other_pid)
    : other_pid_(other_pid) {
  Init(mode);
  SetPipe(pipe.release());

  EnqueueHelloMessage();
}

void ChannelPosix::SetPipe(int fd) {
  chan_cap_.NoteExclusiveAccess();

  pipe_ = fd;
  pipe_buf_len_ = 0;
  if (fd >= 0) {
    int buf_len;
    socklen_t optlen = sizeof(buf_len);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_len, &optlen) != 0) {
      CHROMIUM_LOG(WARNING)
          << "Unable to determine pipe buffer size: " << strerror(errno);
      return;
    }
    CHECK(optlen == sizeof(buf_len));
    CHECK(buf_len > 0);
    pipe_buf_len_ = static_cast<unsigned>(buf_len);
  }
}

bool ChannelPosix::PipeBufHasSpaceAfter(size_t already_written) {
  return pipe_buf_len_ == 0 ||
         static_cast<size_t>(pipe_buf_len_) > already_written;
}

void ChannelPosix::Init(Mode mode) {
  static_assert(sizeof(*this) <= 512, "Exceeded expected size class");

  MOZ_RELEASE_ASSERT(kControlBufferHeaderSize >= CMSG_SPACE(0));
  MOZ_RELEASE_ASSERT(kControlBufferSize >=
                     CMSG_SPACE(sizeof(int) * kControlBufferMaxFds));

  chan_cap_.NoteExclusiveAccess();

  mode_ = mode;
  is_blocked_on_write_ = false;
  partial_write_.reset();
  input_buf_offset_ = 0;
  input_buf_ = mozilla::MakeUnique<char[]>(Channel::kReadBufferSize);
  input_cmsg_buf_ = mozilla::MakeUnique<char[]>(kControlBufferSize);
  SetPipe(-1);
  waiting_connect_ = true;
}

bool ChannelPosix::EnqueueHelloMessage() {
  mozilla::UniquePtr<Message> msg(
      new Message(MSG_ROUTING_NONE, HELLO_MESSAGE_TYPE));
  if (!msg->WriteInt(base::GetCurrentProcId())) {
    CloseLocked();
    return false;
  }

  OutputQueuePush(std::move(msg));
  return true;
}

bool ChannelPosix::Connect(Listener* listener) {
  IOThread().AssertOnCurrentThread();
  mozilla::MutexAutoLock lock(SendMutex());
  chan_cap_.NoteExclusiveAccess();

  if (pipe_ == -1) {
    return false;
  }

  listener_ = listener;

  return ContinueConnect();
}

bool ChannelPosix::ContinueConnect() {
  chan_cap_.NoteExclusiveAccess();
  MOZ_ASSERT(pipe_ != -1);


  MessageLoopForIO::current()->WatchFileDescriptor(
      pipe_, true, MessageLoopForIO::WATCH_READ, &read_watcher_, this);
  waiting_connect_ = false;

  return ProcessOutgoingMessages();
}

void ChannelPosix::SetOtherPid(base::ProcessId other_pid) {
  IOThread().AssertOnCurrentThread();
  mozilla::MutexAutoLock lock(SendMutex());
  chan_cap_.NoteExclusiveAccess();
  MOZ_RELEASE_ASSERT(
      other_pid_ == base::kInvalidProcessId || other_pid_ == other_pid,
      "Multiple sources of SetOtherPid disagree!");
  other_pid_ = other_pid;
}

bool ChannelPosix::ProcessIncomingMessages() {
  chan_cap_.NoteOnTarget();

  struct msghdr msg = {nullptr};
  struct iovec iov;

  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = input_cmsg_buf_.get();

  for (;;) {
    msg.msg_controllen = kControlBufferSize;

    if (pipe_ == -1) return false;

    iov.iov_base = input_buf_.get() + input_buf_offset_;
    iov.iov_len = Channel::kReadBufferSize - input_buf_offset_;

    int recvFlags = MSG_DONTWAIT;
#if defined(MSG_CMSG_CLOEXEC)
    recvFlags |= MSG_CMSG_CLOEXEC;
#endif
    ssize_t bytes_read = HANDLE_EINTR(recvmsg(pipe_, &msg, recvFlags));

    if (bytes_read < 0) {
      if (errno == EAGAIN) {
        return true;
      } else {
        if (!ErrorIsBrokenPipe(errno)) {
          CHROMIUM_LOG(ERROR)
              << "pipe error (fd " << pipe_ << "): " << strerror(errno);
        }
        return false;
      }
    } else if (bytes_read == 0) {
      Close();
      return false;
    }
    DCHECK(bytes_read);

    const int* wire_fds = nullptr;
    unsigned num_wire_fds = 0;


    if (msg.msg_controllen > 0) {
      for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg;
           cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
          const unsigned payload_len = cmsg->cmsg_len - CMSG_LEN(0);
          DCHECK(payload_len % sizeof(int) == 0);
          wire_fds = reinterpret_cast<int*>(CMSG_DATA(cmsg));
          num_wire_fds = payload_len / 4;

          if (msg.msg_flags & MSG_CTRUNC) {
            CHROMIUM_LOG(ERROR)
                << "SCM_RIGHTS message was truncated"
                << " cmsg_len:" << cmsg->cmsg_len << " fd:" << pipe_;
            for (unsigned i = 0; i < num_wire_fds; ++i)
              IGNORE_EINTR(close(wire_fds[i]));
            return false;
          }
          break;
        }
      }
    }

    const char* p = input_buf_.get();
    const char* end = input_buf_.get() + input_buf_offset_ + bytes_read;

    const int* fds;
    unsigned num_fds;
    unsigned fds_i = 0;  

    if (input_overflow_fds_.empty()) {
      fds = wire_fds;
      num_fds = num_wire_fds;
    } else {
      if (num_wire_fds > 0) {
        const size_t prev_size = input_overflow_fds_.size();
        input_overflow_fds_.resize(prev_size + num_wire_fds);
        memcpy(&input_overflow_fds_[prev_size], wire_fds,
               num_wire_fds * sizeof(int));
      }
      fds = &input_overflow_fds_[0];
      num_fds = input_overflow_fds_.size();
    }


    while (p < end && pipe_ != -1) {
      uint32_t message_length = 0;
      if (incoming_message_) {
        message_length = incoming_message_->size();
      } else {
        message_length = Message::MessageSize(p, end);
      }

      if (!message_length) {
        MOZ_ASSERT(!incoming_message_);

        memmove(input_buf_.get(), p, end - p);
        input_buf_offset_ = end - p;

        break;
      }

      input_buf_offset_ = 0;

      bool partial;
      if (incoming_message_) {
        Message& m = *incoming_message_;

        MOZ_DIAGNOSTIC_ASSERT(message_length > m.CurrentSize());
        uint32_t remaining = message_length - m.CurrentSize();

        uint32_t in_buf = std::min(remaining, uint32_t(end - p));

        m.InputBytes(p, in_buf);
        p += in_buf;

        partial = in_buf != remaining;
      } else {
        uint32_t in_buf = std::min(message_length, uint32_t(end - p));

        incoming_message_ = mozilla::MakeUnique<Message>(p, in_buf);
        p += in_buf;

        partial = in_buf != message_length;
      }

      if (partial) {
        break;
      }

      Message& m = *incoming_message_;

      if (m.header()->num_handles) {
        const char* error = nullptr;
        if (m.header()->num_handles > num_fds - fds_i) {
          error = "Message needs unreceived descriptors";
        }

        size_t maxHandles = std::min<size_t>(
            m.size(), IPC::Message::MAX_DESCRIPTORS_PER_MESSAGE);
        if (m.header()->num_handles > maxHandles) {
          error = "Message requires an excessive number of descriptors";
        }

        if (error) {
          CHROMIUM_LOG(WARNING)
              << error << " channel:" << this << " message-type:" << m.type()
              << " header()->num_handles:" << m.header()->num_handles
              << " num_fds:" << num_fds << " fds_i:" << fds_i;
          for (unsigned i = fds_i; i < num_fds; ++i)
            IGNORE_EINTR(close(fds[i]));
          input_overflow_fds_.clear();
          return false;
        }


        nsTArray<mozilla::UniqueFileHandle> handles(m.header()->num_handles);
        for (unsigned end_i = fds_i + m.header()->num_handles; fds_i < end_i;
             ++fds_i) {
          mozilla::UniqueFileHandle fh(fds[fds_i]);
#if !defined(MSG_CMSG_CLOEXEC)
          mozilla::SetCloseOnExec(fh);
#endif
          handles.AppendElement(std::move(fh));
        }
        m.SetAttachedFileHandles(std::move(handles));
      }


#if defined(IPC_MESSAGE_DEBUG_EXTRA)
      DLOG(INFO) << "received message on channel @" << this << " with type "
                 << m.type();
#endif

      if (m.routing_id() == MSG_ROUTING_NONE &&
          m.type() == HELLO_MESSAGE_TYPE) {
        int32_t other_pid = MessageIterator(m).NextInt();
        SetOtherPid(other_pid);
        listener_->OnChannelConnected(other_pid);
      } else {
        mozilla::LogIPCMessage::Run run(&m);
        listener_->OnMessageReceived(std::move(incoming_message_));
      }

      incoming_message_ = nullptr;
    }

    input_overflow_fds_ = std::vector<int>(&fds[fds_i], &fds[num_fds]);

    if (!incoming_message_ && input_buf_offset_ == 0 &&
        !input_overflow_fds_.empty()) {
      return false;
    }
  }
}

bool ChannelPosix::ProcessOutgoingMessages() {
  chan_cap_.NoteLockHeld();

  DCHECK(!waiting_connect_);  
  is_blocked_on_write_ = false;

  if (output_queue_.IsEmpty()) return true;

  if (pipe_ == -1) return false;

  while (!output_queue_.IsEmpty()) {
    Message* msg = output_queue_.FirstElement().get();

    struct msghdr msgh = {nullptr};

    char cmsgBuf[kControlBufferSize];

    if (partial_write_.isNothing()) {

      size_t maxHandles = std::min<size_t>(
          msg->size(), IPC::Message::MAX_DESCRIPTORS_PER_MESSAGE);
      if (msg->attached_handles_.Length() > maxHandles) {
        MOZ_DIAGNOSTIC_CRASH("Too many file descriptors!");
        CHROMIUM_LOG(FATAL) << "Too many file descriptors!";
        return false;
      }

      msg->header()->num_handles = msg->attached_handles_.Length();

      Pickle::BufferList::IterImpl iter(msg->Buffers());
      MOZ_DIAGNOSTIC_ASSERT(!iter.Done(), "empty message");
      partial_write_.emplace(PartialWrite{iter, msg->attached_handles_});

    }

    if (partial_write_->iter_.Done()) {
      MOZ_DIAGNOSTIC_CRASH("partial_write_->iter_ should not be done");
      return false;
    }

    Pickle::BufferList::IterImpl iter = partial_write_->iter_;
    auto handles = partial_write_->handles_;

    const size_t num_fds = std::min(handles.Length(), kControlBufferMaxFds);
    size_t max_amt_to_write = iter.TotalBytesAvailable(msg->Buffers());
    if (num_fds > 0) {
      msgh.msg_control = cmsgBuf;
      msgh.msg_controllen = CMSG_LEN(sizeof(int) * num_fds);
      struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msgh);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = msgh.msg_controllen;
      for (size_t i = 0; i < num_fds; ++i) {
        reinterpret_cast<int*>(CMSG_DATA(cmsg))[i] = handles[i].get();
      }

      size_t remaining = handles.Length() - num_fds;
      MOZ_ASSERT(max_amt_to_write > remaining,
                 "must be at least one byte in the message for each handle");
      max_amt_to_write -= remaining;
    }

    struct iovec iov[kMaxIOVecSize];
    size_t iov_count = 0;
    size_t amt_to_write = 0;
    while (!iter.Done() && iov_count < kMaxIOVecSize &&
           PipeBufHasSpaceAfter(amt_to_write) &&
           amt_to_write < max_amt_to_write) {
      char* data = iter.Data();
      size_t size =
          std::min(iter.RemainingInSegment(), max_amt_to_write - amt_to_write);

      iov[iov_count].iov_base = data;
      iov[iov_count].iov_len = size;
      iov_count++;
      amt_to_write += size;
      iter.Advance(msg->Buffers(), size);
    }
    MOZ_ASSERT(amt_to_write <= max_amt_to_write);
    MOZ_ASSERT(amt_to_write > 0);

    const bool intentional_short_write = !iter.Done();
    msgh.msg_iov = iov;
    msgh.msg_iovlen = iov_count;

    ssize_t bytes_written =
        HANDLE_EINTR(corrected_sendmsg(pipe_, &msgh, MSG_DONTWAIT));

    if (bytes_written < 0) {
      switch (errno) {
        case EAGAIN:
          break;
        default:
          if (!ErrorIsBrokenPipe(errno)) {
            CHROMIUM_LOG(ERROR) << "pipe error: " << strerror(errno);
          }
          return false;
      }
    }

    if (intentional_short_write ||
        static_cast<size_t>(bytes_written) != amt_to_write) {
      if (bytes_written > 0) {
        MOZ_DIAGNOSTIC_ASSERT(intentional_short_write ||
                              static_cast<size_t>(bytes_written) <
                                  amt_to_write);
        partial_write_->iter_.AdvanceAcrossSegments(msg->Buffers(),
                                                    bytes_written);
        partial_write_->handles_ = handles.From(num_fds);
        MOZ_DIAGNOSTIC_ASSERT(!partial_write_->iter_.Done());
      }

      is_blocked_on_write_ = true;
      if (IOThread().IsOnCurrentThread()) {
        MessageLoopForIO::current()->WatchFileDescriptor(
            pipe_,
            false,  
            MessageLoopForIO::WATCH_WRITE, &write_watcher_, this);
      } else {
        IOThread().Dispatch(mozilla::NewRunnableMethod<int>(
            "ChannelPosix::ContinueProcessOutgoing", this,
            &ChannelPosix::OnFileCanWriteWithoutBlocking, -1));
      }
      return true;
    } else {
      MOZ_ASSERT(partial_write_->handles_.Length() == num_fds,
                 "not all handles were sent");
      partial_write_.reset();

      if (bytes_written > 0) {
        msg->attached_handles_.Clear();
      }



#if defined(IPC_MESSAGE_DEBUG_EXTRA)
      DLOG(INFO) << "sent message @" << msg << " on channel @" << this
                 << " with type " << msg->type();
#endif
      OutputQueuePop();
      msg = nullptr;
    }
  }
  return true;
}

bool ChannelPosix::Send(mozilla::UniquePtr<Message> message) {
  mozilla::MutexAutoLock lock(SendMutex());
  chan_cap_.NoteLockHeld();

#if defined(IPC_MESSAGE_DEBUG_EXTRA)
  DLOG(INFO) << "sending message @" << message.get() << " on channel @" << this
             << " with type " << message->type() << " ("
             << output_queue_.Count() << " in queue)";
#endif

  if (pipe_ == -1) {
    if (mozilla::ipc::LoggingEnabled()) {
      fprintf(stderr,
              "Can't send message %s, because this channel is closed.\n",
              message->name());
    }
    return false;
  }

  OutputQueuePush(std::move(message));
  if (!waiting_connect_) {
    if (!is_blocked_on_write_) {
      if (!ProcessOutgoingMessages()) return false;
    }
  }

  return true;
}

void ChannelPosix::OnFileCanReadWithoutBlocking(int fd) {
  IOThread().AssertOnCurrentThread();
  chan_cap_.NoteOnTarget();

  if (!waiting_connect_ && fd == pipe_ && pipe_ != -1) {
    if (!ProcessIncomingMessages()) {
      Close();
      listener_->OnChannelError();
      return;
    }
  }
}


void ChannelPosix::OutputQueuePush(mozilla::UniquePtr<Message> msg) {
  chan_cap_.NoteLockHeld();

  mozilla::LogIPCMessage::LogDispatchWithPid(msg.get(), other_pid_);

  MOZ_DIAGNOSTIC_ASSERT(pipe_ != -1);
  msg->AssertAsLargeAsHeader();
  output_queue_.Push(std::move(msg));
}

void ChannelPosix::OutputQueuePop() {
  partial_write_.reset();

  mozilla::UniquePtr<Message> message = output_queue_.Pop();
}

void ChannelPosix::OnFileCanWriteWithoutBlocking(int fd) {
  RefPtr<ChannelPosix> grip(this);
  IOThread().AssertOnCurrentThread();
  mozilla::ReleasableMutexAutoLock lock(SendMutex());
  chan_cap_.NoteExclusiveAccess();
  if (pipe_ != -1 && !ProcessOutgoingMessages()) {
    CloseLocked();
    lock.Unlock();
    listener_->OnChannelError();
  }
}

void ChannelPosix::Close() {
  IOThread().AssertOnCurrentThread();
  mozilla::MutexAutoLock lock(SendMutex());
  CloseLocked();
}

void ChannelPosix::CloseLocked() {
  chan_cap_.NoteExclusiveAccess();


  read_watcher_.StopWatchingFileDescriptor();
  write_watcher_.StopWatchingFileDescriptor();
  if (pipe_ != -1) {
    IGNORE_EINTR(close(pipe_));
    SetPipe(-1);
  }

  while (!output_queue_.IsEmpty()) {
    OutputQueuePop();
  }

  for (std::vector<int>::iterator i = input_overflow_fds_.begin();
       i != input_overflow_fds_.end(); ++i) {
    IGNORE_EINTR(close(*i));
  }
  input_overflow_fds_.clear();

}


bool ChannelPosix::CreateRawPipe(ChannelHandle* server, ChannelHandle* client) {
  int fds[2];
  int type = SOCK_STREAM;
#if defined(SOCK_CLOEXEC)
  type |= SOCK_CLOEXEC;
#endif
#if defined(SOCK_NONBLOCK)
  type |= SOCK_NONBLOCK;
#endif

  if (socketpair(AF_UNIX, type, 0, fds) < 0) {
    return false;
  }

  auto configureFd = [](int fd) -> bool {
#if !defined(SOCK_NONBLOCK)
    int flFlags = fcntl(fd, F_GETFL);
    if (flFlags == -1) {
      return false;
    }
    if (fcntl(fd, F_SETFL, flFlags | O_NONBLOCK) == -1) {
      return false;
    }
#endif

#if !defined(SOCK_CLOEXEC)
    int fdFlags = fcntl(fd, F_GETFD);
    if (fdFlags == -1) {
      return false;
    }
    if (fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC) == -1) {
      return false;
    }
#endif
    return true;
  };

  if (!configureFd(fds[0]) || !configureFd(fds[1])) {
    IGNORE_EINTR(close(fds[0]));
    IGNORE_EINTR(close(fds[1]));
    return false;
  }

  server->emplace<mozilla::UniqueFileHandle>(fds[0]);
  client->emplace<mozilla::UniqueFileHandle>(fds[1]);
  return true;
}

uint32_t ChannelPosix::NumRelayedAttachments(const Message& message) {
  return 0;
}

bool ChannelPosix::IsValidHandle(const ChannelHandle& handle) {
  const auto* fileHandle = std::get_if<mozilla::UniqueFileHandle>(&handle);
  return fileHandle && *fileHandle;
}

}  
