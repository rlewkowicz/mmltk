/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AsyncPlatformPipes.h"

#include "base/eintr_wrapper.h"
#include "base/message_loop.h"
#include "mozilla/CondVar.h"
#include "mozilla/EventTargetCapability.h"
#include "mozilla/UniquePtr.h"
#include "nsStreamUtils.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"

#  include <errno.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>

namespace mozilla {

namespace platform_pipe_detail {

class PlatformPipeLink
    : public MessageLoopForIO::Watcher
{
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(PlatformPipeLink)

 public:
  PlatformPipeLink(UniqueFileHandle aHandle, uint32_t aBufferSize);

  already_AddRefed<PlatformPipeLink> TakePending() MOZ_REQUIRES(mMutex);

  void Close(nsresult aStatus, bool aInternal) MOZ_EXCLUDES(mMutex)
      MOZ_EXCLUDES(mIOThread);

  void DispatchPipeError(nsresult aStatus) MOZ_REQUIRES(mMutex, mIOThread);

  void DispatchNotify() MOZ_REQUIRES(mMutex);

  void AdvanceIO() MOZ_EXCLUDES(mMutex) MOZ_REQUIRES(mIOThread);

  void AdvanceIOLocked() MOZ_REQUIRES(mMutex, mIOThread);

  void OnFileCanReadWithoutBlocking(int fd) override;
  void OnFileCanWriteWithoutBlocking(int fd) override;

  const EventTargetCapability<nsISerialEventTarget> mIOThread;
  Mutex mMutex{"PlatformPipeReader"};

  UniqueFileHandle mHandle MOZ_GUARDED_BY(mMutex);
  const UniquePtr<char[]> mBuffer;
  const uint32_t mBufferSize;

  bool mProcessingSegment MOZ_GUARDED_BY(mMutex) = false;
  bool mClosing MOZ_GUARDED_BY(mMutex) = false;

  nsresult mStatus MOZ_GUARDED_BY(mMutex) = NS_OK;

  uint32_t mOffset MOZ_GUARDED_BY(mMutex) = 0;
  uint32_t mAvailable MOZ_GUARDED_BY(mMutex) = 0;

  bool mCallbackClosureOnly MOZ_GUARDED_BY(mMutex) = false;
  nsCOMPtr<nsIRunnable> mCallback MOZ_GUARDED_BY(mMutex);
  nsCOMPtr<nsIEventTarget> mCallbackTarget MOZ_GUARDED_BY(mMutex);

  RefPtr<PlatformPipeLink> mPending MOZ_GUARDED_BY(mMutex);

  CondVar mPendingCV{mMutex, "PlatformPipeReader::mPendingCV"};

  MessageLoopForIO::FileDescriptorWatcher mWatcher MOZ_GUARDED_BY(mIOThread);

 private:
  ~PlatformPipeLink() = default;
};

PlatformPipeLink::PlatformPipeLink(UniqueFileHandle aHandle,
                                   uint32_t aBufferSize)
    : mIOThread(XRE_GetAsyncIOEventTarget()),
      mHandle(std::move(aHandle)),
      mBuffer(MakeUnique<char[]>(aBufferSize)),
      mBufferSize(aBufferSize) {
  MOZ_ASSERT(aBufferSize > 1, "invalid buffer size");
  MOZ_ASSERT(mHandle, "invalid handle");

#if defined(DEBUG) && !0
  struct stat st{};
  MOZ_ASSERT(fstat(mHandle.get(), &st) == 0 && !S_ISREG(st.st_mode),
             "PlatformPipeLink does not support regular files");
  MOZ_ASSERT(fcntl(mHandle.get(), F_GETFL) & O_NONBLOCK,
             "PlatformPipeLink requires non-blocking file descriptors");
#endif
}

already_AddRefed<PlatformPipeLink> PlatformPipeLink::TakePending() {
  RefPtr<PlatformPipeLink> pending = mPending.forget();
  if (pending) {
    mPendingCV.NotifyAll();
  }
  return pending.forget();
}

void PlatformPipeLink::Close(nsresult aStatus, bool aInternal) {
  MOZ_RELEASE_ASSERT(!mIOThread.IsOnCurrentThread(),
                     "Close may deadlock if called on the IO thread");

  MutexAutoLock lock(mMutex);
  MOZ_RELEASE_ASSERT(aInternal || !mProcessingSegment,
                     "Cannot close pipe during ReadSegments callback");
  if (NS_FAILED(mStatus)) {
    return;
  }

  mClosing = true;

  if (mPending) {
    MOZ_ALWAYS_SUCCEEDS(mIOThread.Dispatch(NS_NewRunnableFunction(
        "PlatformPipeLink::CancelIO", [self = RefPtr{this}] {
          self->mIOThread.AssertOnCurrentThread();
          RefPtr<PlatformPipeLink> pending;
          MutexAutoLock lock(self->mMutex);
          if (self->mPending) {
            self->mWatcher.StopWatchingFileDescriptor();
            pending = self->TakePending();
          }
        })));

    while (mPending) {
      mPendingCV.Wait();
    }
  }
  MOZ_DIAGNOSTIC_ASSERT(!mPending, "How do we still have pending I/O?");

  if (NS_FAILED(mStatus)) {
    return;
  }

  mStatus = NS_SUCCEEDED(aStatus) ? NS_BASE_STREAM_CLOSED : aStatus;
  DispatchNotify();

  mHandle = nullptr;
}

void PlatformPipeLink::DispatchPipeError(nsresult aStatus) {
  MOZ_ASSERT(!mPending,
             "Shouldn't be pending when closing due to a pipe error");

  mClosing = true;

  NS_DispatchBackgroundTask(
      NewRunnableMethod<nsresult, bool>("PlatformPipeLink::Close", this,
                                        &PlatformPipeLink::Close, aStatus,
                                         true),
      NS_DISPATCH_EVENT_MAY_BLOCK);
}

void PlatformPipeLink::DispatchNotify() {
  nsCOMPtr<nsIRunnable> callback = mCallback.forget();
  nsCOMPtr<nsIEventTarget> target = mCallbackTarget.forget();
  if (!callback) {
    return;
  }
  if (target) {
    target->Dispatch(callback.forget());
  } else {
    NS_DispatchBackgroundTask(callback.forget());
  }
}

void PlatformPipeLink::AdvanceIO() {
  MutexAutoLock lock(mMutex);
  AdvanceIOLocked();
}

void PlatformPipeLink::AdvanceIOLocked() {
  if (mClosing || !mHandle || NS_FAILED(mStatus)) {
    return;
  }

  if (mPending || mAvailable) {
    return;
  }

  ssize_t rv = HANDLE_EINTR(read(mHandle.get(), mBuffer.get(), mBufferSize));
  if (rv > 0) {
    mOffset = 0;
    mAvailable = static_cast<uint32_t>(rv);
    if (!mCallbackClosureOnly) {
      DispatchNotify();
    }
    return;
  }

  if (rv == 0) {
    DispatchPipeError(NS_BASE_STREAM_CLOSED);
    return;
  }

  if (errno == EAGAIN
#if EWOULDBLOCK != EAGAIN
      || errno == EWOULDBLOCK
#endif
  ) {
    if (MessageLoopForIO::current()->WatchFileDescriptor(
            mHandle.get(), false, MessageLoopForIO::WATCH_READ, &mWatcher,
            this)) {
      mPending = this;
      return;
    }
  }

  DispatchPipeError(NS_ERROR_FAILURE);
}

void PlatformPipeLink::OnFileCanReadWithoutBlocking(int fd) {
  mIOThread.AssertOnCurrentThread();
  RefPtr<PlatformPipeLink> pending;
  MutexAutoLock lock(mMutex);
  pending = TakePending();
  AdvanceIOLocked();
}

void PlatformPipeLink::OnFileCanWriteWithoutBlocking(int fd) {
  MOZ_ASSERT_UNREACHABLE();
}

}  


NS_IMPL_ISUPPORTS(PlatformPipeReader, nsIInputStream, nsIAsyncInputStream)

PlatformPipeReader::PlatformPipeReader(UniqueFileHandle aHandle,
                                       uint32_t aBufferSize)
    : mLink(new platform_pipe_detail::PlatformPipeLink(std::move(aHandle),
                                                       aBufferSize)) {}

PlatformPipeReader::~PlatformPipeReader() { Close(); }

NS_IMETHODIMP PlatformPipeReader::Close() {
  return CloseWithStatus(NS_BASE_STREAM_CLOSED);
}

NS_IMETHODIMP PlatformPipeReader::Available(uint64_t* aAvailable) {
  MutexAutoLock lock(mLink->mMutex);
  if (NS_FAILED(mLink->mStatus)) {
    return mLink->mStatus;
  }
  *aAvailable = mLink->mAvailable;
  return NS_OK;
}

NS_IMETHODIMP PlatformPipeReader::StreamStatus() {
  MutexAutoLock lock(mLink->mMutex);
  return mLink->mStatus;
}

NS_IMETHODIMP PlatformPipeReader::Read(char* aBuf, uint32_t aCount,
                                       uint32_t* aReadCount) {
  return ReadSegments(NS_CopySegmentToBuffer, aBuf, aCount, aReadCount);
}

NS_IMETHODIMP PlatformPipeReader::ReadSegments(nsWriteSegmentFun aWriter,
                                               void* aClosure, uint32_t aCount,
                                               uint32_t* aReadCount) {
  *aReadCount = 0;

  MutexAutoLock lock(mLink->mMutex);
  if (NS_FAILED(mLink->mStatus)) {
    return mLink->mStatus == NS_BASE_STREAM_CLOSED ? NS_OK : mLink->mStatus;
  }

  if (!mLink->mAvailable) {
    return NS_BASE_STREAM_WOULD_BLOCK;
  }

  MOZ_RELEASE_ASSERT(!mLink->mProcessingSegment,
                     "Only one thread may be processing a segment at a time");

  char* start = mLink->mBuffer.get() + mLink->mOffset;
  uint32_t length = std::min(aCount, mLink->mAvailable);

  mLink->mProcessingSegment = true;
  {
    MutexAutoUnlock unlock(mLink->mMutex);
    nsresult rv = aWriter(this, aClosure, start, 0, length, aReadCount);
    if (NS_FAILED(rv)) {
      *aReadCount = 0;
    }
    MOZ_RELEASE_ASSERT(*aReadCount <= length);
  }
  mLink->mProcessingSegment = false;

  mLink->mOffset += *aReadCount;
  mLink->mAvailable -= *aReadCount;

  if (!mLink->mAvailable && mLink->mCallback && mLink->mCallbackClosureOnly) {
    mLink->mIOThread.Dispatch(
        NewRunnableMethod("PlatformPipeLink::AdvanceIO", mLink,
                          &platform_pipe_detail::PlatformPipeLink::AdvanceIO));
  }
  return NS_OK;
}

NS_IMETHODIMP PlatformPipeReader::IsNonBlocking(bool* aNonBlocking) {
  *aNonBlocking = true;
  return NS_OK;
}

NS_IMETHODIMP PlatformPipeReader::CloseWithStatus(nsresult aStatus) {
  mLink->Close(aStatus,  false);
  return NS_OK;
}

NS_IMETHODIMP PlatformPipeReader::AsyncWait(nsIInputStreamCallback* aCallback,
                                            uint32_t aFlags,
                                            uint32_t aRequestedCount,
                                            nsIEventTarget* aTarget) {
  MutexAutoLock lock(mLink->mMutex);

  if (!aCallback) {
    mLink->mCallback = nullptr;
    mLink->mCallbackTarget = nullptr;
    return NS_OK;
  }

  mLink->mCallback = NS_NewRunnableFunction(
      "PlatformPipeReader::AsyncWait",
      [self = RefPtr{this}, callback = RefPtr{aCallback}] {
        callback->OnInputStreamReady(self);
      });
  mLink->mCallbackTarget = aTarget;
  mLink->mCallbackClosureOnly = aFlags & WAIT_CLOSURE_ONLY;

  if (NS_FAILED(mLink->mStatus) ||
      (!mLink->mCallbackClosureOnly && mLink->mAvailable)) {
    mLink->DispatchNotify();
  } else {
    mLink->mIOThread.Dispatch(
        NewRunnableMethod("PlatformPipeLink::AdvanceIO", mLink,
                          &platform_pipe_detail::PlatformPipeLink::AdvanceIO));
  }
  return NS_OK;
}

}  
