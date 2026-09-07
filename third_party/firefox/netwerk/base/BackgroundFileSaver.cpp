/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundFileSaver.h"

#include "mozilla/Logging.h"
#include "nsComponentManagerUtils.h"
#include "nsIAsyncInputStream.h"
#include "nsIFile.h"
#include "nsIPipe.h"
#include "nsNetUtil.h"
#include "nsThreadUtils.h"
#include "prio.h"

namespace mozilla {
namespace net {

static LazyLogModule prlog("BackgroundFileSaver");
#define LOG(args) MOZ_LOG(prlog, mozilla::LogLevel::Debug, args)


#define BUFFERED_IO_SIZE (1024 * 32)

#define REQUEST_SUSPEND_AT (1024 * 1024 * 4)

#define REQUEST_RESUME_AT (1024 * 1024 * 2)


class NotifyTargetChangeRunnable final : public Runnable {
 public:
  NotifyTargetChangeRunnable(BackgroundFileSaver* aSaver, nsIFile* aTarget)
      : Runnable("net::NotifyTargetChangeRunnable"),
        mSaver(aSaver),
        mTarget(aTarget) {}

  NS_IMETHOD Run() override { return mSaver->NotifyTargetChange(mTarget); }

 private:
  RefPtr<BackgroundFileSaver> mSaver;
  nsCOMPtr<nsIFile> mTarget;
};


uint32_t BackgroundFileSaver::sThreadCount = 0;
uint32_t BackgroundFileSaver::sTelemetryMaxThreadCount = 0;

BackgroundFileSaver::BackgroundFileSaver() {
  LOG(("Created BackgroundFileSaver [this = %p]", this));
}

BackgroundFileSaver::~BackgroundFileSaver() {
  LOG(("Destroying BackgroundFileSaver [this = %p]", this));
}

nsresult BackgroundFileSaver::Init() {
  MOZ_ASSERT(NS_IsMainThread(), "This should be called on the main thread");

  NS_NewPipe2(getter_AddRefs(mPipeInputStream),
              getter_AddRefs(mPipeOutputStream), true, true, 0,
              HasInfiniteBuffer() ? UINT32_MAX : 0);

  mControlEventTarget = GetCurrentSerialEventTarget();
  NS_ENSURE_TRUE(mControlEventTarget, NS_ERROR_NOT_INITIALIZED);

  nsresult rv = NS_CreateBackgroundTaskQueue("BgFileSaver",
                                             getter_AddRefs(mBackgroundET));
  NS_ENSURE_SUCCESS(rv, rv);

  sThreadCount++;
  if (sThreadCount > sTelemetryMaxThreadCount) {
    sTelemetryMaxThreadCount = sThreadCount;
  }

  return NS_OK;
}

NS_IMETHODIMP
BackgroundFileSaver::GetObserver(nsIBackgroundFileSaverObserver** aObserver) {
  NS_ENSURE_ARG_POINTER(aObserver);
  *aObserver = do_AddRef(mObserver).take();
  return NS_OK;
}

NS_IMETHODIMP
BackgroundFileSaver::SetObserver(nsIBackgroundFileSaverObserver* aObserver) {
  mObserver = aObserver;
  return NS_OK;
}

NS_IMETHODIMP
BackgroundFileSaver::EnableAppend() {
  MOZ_ASSERT(NS_IsMainThread(), "This should be called on the main thread");

  MutexAutoLock lock(mLock);
  mAppend = true;

  return NS_OK;
}

NS_IMETHODIMP
BackgroundFileSaver::SetTarget(nsIFile* aTarget, bool aKeepPartial) {
  NS_ENSURE_ARG(aTarget);
  {
    MutexAutoLock lock(mLock);
    if (!mInitialTarget) {
      aTarget->Clone(getter_AddRefs(mInitialTarget));
      mInitialTargetKeepPartial = aKeepPartial;
    } else {
      aTarget->Clone(getter_AddRefs(mRenamedTarget));
      mRenamedTargetKeepPartial = aKeepPartial;
    }
  }

  return GetWorkerThreadAttention(true);
}

NS_IMETHODIMP
BackgroundFileSaver::Finish(nsresult aStatus) {
  nsresult rv;

  rv = mPipeOutputStream->Close();
  NS_ENSURE_SUCCESS(rv, rv);

  {
    MutexAutoLock lock(mLock);
    mFinishRequested = true;
    if (NS_SUCCEEDED(mStatus)) {
      mStatus = aStatus;
    }
  }

  return GetWorkerThreadAttention(NS_FAILED(aStatus));
}

nsresult BackgroundFileSaver::GetWorkerThreadAttention(
    bool aShouldInterruptCopy) {
  nsresult rv;

  MutexAutoLock lock(mLock);

  if (mWorkerThreadAttentionRequested) {
    return NS_OK;
  }

  if (!mAsyncCopyContext) {
    if (!mBackgroundET) {
      return NS_ERROR_UNEXPECTED;
    }

    rv = mBackgroundET->Dispatch(
        NewRunnableMethod("net::BackgroundFileSaver::ProcessAttention", this,
                          &BackgroundFileSaver::ProcessAttention),
        NS_DISPATCH_EVENT_MAY_BLOCK);
    NS_ENSURE_SUCCESS(rv, rv);

  } else if (aShouldInterruptCopy) {
    NS_CancelAsyncCopy(mAsyncCopyContext, NS_ERROR_ABORT);
  }

  mWorkerThreadAttentionRequested = true;

  return NS_OK;
}

void BackgroundFileSaver::AsyncCopyCallback(void* aClosure, nsresult aStatus) {
  RefPtr<BackgroundFileSaver> self =
      dont_AddRef((BackgroundFileSaver*)aClosure);
  {
    MutexAutoLock lock(self->mLock);

    self->mAsyncCopyContext = nullptr;

    if (NS_FAILED(aStatus) && aStatus != NS_ERROR_ABORT &&
        NS_SUCCEEDED(self->mStatus)) {
      self->mStatus = aStatus;
    }
  }

  (void)self->ProcessAttention();
}

nsresult BackgroundFileSaver::ProcessAttention() {
  nsresult rv;


  MOZ_ASSERT(!NS_IsMainThread());
  {
    MutexAutoLock lock(mLock);
    if (mAsyncCopyContext) {
      NS_CancelAsyncCopy(mAsyncCopyContext, NS_ERROR_ABORT);
      return NS_OK;
    }
  }
  rv = ProcessStateChange();
  if (NS_FAILED(rv)) {
    {
      MutexAutoLock lock(mLock);

      if (NS_SUCCEEDED(mStatus)) {
        mStatus = rv;
      }
    }
    CheckCompletion();
  }

  return NS_OK;
}

nsresult BackgroundFileSaver::ProcessStateChange() {
  nsresult rv;

  if (CheckCompletion()) {
    return NS_OK;
  }

  nsCOMPtr<nsIFile> initialTarget;
  bool initialTargetKeepPartial;
  nsCOMPtr<nsIFile> renamedTarget;
  bool renamedTargetKeepPartial;
  bool append;
  {
    MutexAutoLock lock(mLock);

    initialTarget = mInitialTarget;
    initialTargetKeepPartial = mInitialTargetKeepPartial;
    renamedTarget = mRenamedTarget;
    renamedTargetKeepPartial = mRenamedTargetKeepPartial;
    append = mAppend;

    mWorkerThreadAttentionRequested = false;
  }

  if (!initialTarget) {
    return NS_OK;
  }

  bool isContinuation = !!mActualTarget;
  if (!isContinuation) {
    mActualTarget = initialTarget;
    mActualTargetKeepPartial = initialTargetKeepPartial;
  }

  bool equalToCurrent = false;
  if (renamedTarget) {
    rv = mActualTarget->Equals(renamedTarget, &equalToCurrent);
    NS_ENSURE_SUCCESS(rv, rv);
    if (!equalToCurrent) {
      bool exists = true;
      if (!isContinuation) {
        rv = mActualTarget->Exists(&exists);
        NS_ENSURE_SUCCESS(rv, rv);
      }
      if (exists) {
        nsCOMPtr<nsIFile> renamedTargetParentDir;
        rv = renamedTarget->GetParent(getter_AddRefs(renamedTargetParentDir));
        NS_ENSURE_SUCCESS(rv, rv);

        nsAutoString renamedTargetName;
        rv = renamedTarget->GetLeafName(renamedTargetName);
        NS_ENSURE_SUCCESS(rv, rv);

        rv = mActualTarget->MoveTo(renamedTargetParentDir, renamedTargetName);
        NS_ENSURE_SUCCESS(rv, rv);
      }

    }

    mActualTarget = renamedTarget;
    mActualTargetKeepPartial = renamedTargetKeepPartial;
  }

  if (!equalToCurrent) {
    nsCOMPtr<nsIFile> actualTargetToNotify;
    rv = mActualTarget->Clone(getter_AddRefs(actualTargetToNotify));
    NS_ENSURE_SUCCESS(rv, rv);

    RefPtr<NotifyTargetChangeRunnable> event =
        new NotifyTargetChangeRunnable(this, actualTargetToNotify);
    NS_ENSURE_TRUE(event, NS_ERROR_FAILURE);

    rv = mControlEventTarget->Dispatch(event, NS_DISPATCH_NORMAL);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (isContinuation) {
    if (CheckCompletion()) {
      return NS_OK;
    }

    uint64_t available;
    rv = mPipeInputStream->Available(&available);
    if (NS_FAILED(rv)) {
      return NS_OK;
    }
  }

  int32_t creationIoFlags;
  if (isContinuation) {
    creationIoFlags = PR_APPEND;
  } else {
    creationIoFlags = (append ? PR_APPEND : PR_TRUNCATE) | PR_CREATE_FILE;
  }

  nsCOMPtr<nsIOutputStream> outputStream;
  rv = NS_NewLocalFileOutputStream(getter_AddRefs(outputStream), mActualTarget,
                                   PR_WRONLY | creationIoFlags, 0600);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIOutputStream> bufferedStream;
  rv = NS_NewBufferedOutputStream(getter_AddRefs(bufferedStream),
                                  outputStream.forget(), BUFFERED_IO_SIZE);
  NS_ENSURE_SUCCESS(rv, rv);
  outputStream = bufferedStream;

  {
    MutexAutoLock lock(mLock);

    rv = NS_AsyncCopy(
        mPipeInputStream, outputStream, mBackgroundET,
        NS_ASYNCCOPY_VIA_READSEGMENTS,
        1024 * 1024,
        AsyncCopyCallback, this, false, true, getter_AddRefs(mAsyncCopyContext),
        GetProgressCallback());
    if (NS_FAILED(rv)) {
      NS_WARNING("NS_AsyncCopy failed.");
      mAsyncCopyContext = nullptr;
      return rv;
    }
  }

  NS_ADDREF_THIS();

  return NS_OK;
}

bool BackgroundFileSaver::CheckCompletion() {
  nsresult rv;

  bool failed = true;
  {
    MutexAutoLock lock(mLock);
    MOZ_ASSERT(!mAsyncCopyContext,
               "Should not be copying when checking completion conditions.");

    if (mComplete) {
      return true;
    }

    if (NS_SUCCEEDED(mStatus)) {
      failed = false;

      if (!mFinishRequested) {
        return false;
      }

      if ((mInitialTarget && !mActualTarget) ||
          (mRenamedTarget && mRenamedTarget != mActualTarget)) {
        return false;
      }

      uint64_t available;
      rv = mPipeInputStream->Available(&available);
      if (NS_SUCCEEDED(rv) && available != 0) {
        return false;
      }
    }

    mComplete = true;
  }

  if (failed && mActualTarget && !mActualTargetKeepPartial) {
    (void)mActualTarget->Remove(false);
  }

  if (NS_FAILED(mControlEventTarget->Dispatch(
          NewRunnableMethod("BackgroundFileSaver::NotifySaveComplete", this,
                            &BackgroundFileSaver::NotifySaveComplete),
          NS_DISPATCH_NORMAL))) {
    NS_WARNING("Unable to post completion event to the control thread.");
  }

  return true;
}

nsresult BackgroundFileSaver::NotifyTargetChange(nsIFile* aTarget) {
  if (mObserver) {
    (void)mObserver->OnTargetChange(this, aTarget);
  }

  return NS_OK;
}

nsresult BackgroundFileSaver::NotifySaveComplete() {
  MOZ_ASSERT(NS_IsMainThread(), "This should be called on the main thread");

  nsresult status;
  {
    MutexAutoLock lock(mLock);
    status = mStatus;
  }

  if (mObserver) {
    (void)mObserver->OnSaveComplete(this, status);
    mObserver = nullptr;
  }

  mBackgroundET = nullptr;

  sThreadCount--;

  if (sThreadCount == 0) {

    sTelemetryMaxThreadCount = 0;
  }

  return NS_OK;
}


NS_IMPL_ISUPPORTS(BackgroundFileSaverOutputStream, nsIBackgroundFileSaver,
                  nsIOutputStream, nsIAsyncOutputStream,
                  nsIOutputStreamCallback)

BackgroundFileSaverOutputStream::BackgroundFileSaverOutputStream()
    : mAsyncWaitCallback(nullptr) {}

bool BackgroundFileSaverOutputStream::HasInfiniteBuffer() { return false; }

nsAsyncCopyProgressFun BackgroundFileSaverOutputStream::GetProgressCallback() {
  return nullptr;
}

NS_IMETHODIMP
BackgroundFileSaverOutputStream::Close() { return mPipeOutputStream->Close(); }

NS_IMETHODIMP
BackgroundFileSaverOutputStream::Flush() { return mPipeOutputStream->Flush(); }

NS_IMETHODIMP
BackgroundFileSaverOutputStream::StreamStatus() {
  return mPipeOutputStream->StreamStatus();
}

NS_IMETHODIMP
BackgroundFileSaverOutputStream::Write(const char* aBuf, uint32_t aCount,
                                       uint32_t* _retval) {
  return mPipeOutputStream->Write(aBuf, aCount, _retval);
}

NS_IMETHODIMP
BackgroundFileSaverOutputStream::WriteFrom(nsIInputStream* aFromStream,
                                           uint32_t aCount, uint32_t* _retval) {
  return mPipeOutputStream->WriteFrom(aFromStream, aCount, _retval);
}

NS_IMETHODIMP
BackgroundFileSaverOutputStream::WriteSegments(nsReadSegmentFun aReader,
                                               void* aClosure, uint32_t aCount,
                                               uint32_t* _retval) {
  return mPipeOutputStream->WriteSegments(aReader, aClosure, aCount, _retval);
}

NS_IMETHODIMP
BackgroundFileSaverOutputStream::IsNonBlocking(bool* _retval) {
  return mPipeOutputStream->IsNonBlocking(_retval);
}

NS_IMETHODIMP
BackgroundFileSaverOutputStream::CloseWithStatus(nsresult reason) {
  return mPipeOutputStream->CloseWithStatus(reason);
}

NS_IMETHODIMP
BackgroundFileSaverOutputStream::AsyncWait(nsIOutputStreamCallback* aCallback,
                                           uint32_t aFlags,
                                           uint32_t aRequestedCount,
                                           nsIEventTarget* aEventTarget) {
  NS_ENSURE_STATE(!mAsyncWaitCallback);

  mAsyncWaitCallback = aCallback;

  return mPipeOutputStream->AsyncWait(this, aFlags, aRequestedCount,
                                      aEventTarget);
}

NS_IMETHODIMP
BackgroundFileSaverOutputStream::OnOutputStreamReady(
    nsIAsyncOutputStream* aStream) {
  NS_ENSURE_STATE(mAsyncWaitCallback);

  nsCOMPtr<nsIOutputStreamCallback> asyncWaitCallback = nullptr;
  asyncWaitCallback.swap(mAsyncWaitCallback);

  return asyncWaitCallback->OnOutputStreamReady(this);
}


NS_IMPL_ISUPPORTS(BackgroundFileSaverStreamListener, nsIBackgroundFileSaver,
                  nsIRequestObserver, nsIStreamListener)

bool BackgroundFileSaverStreamListener::HasInfiniteBuffer() { return true; }

nsAsyncCopyProgressFun
BackgroundFileSaverStreamListener::GetProgressCallback() {
  return AsyncCopyProgressCallback;
}

NS_IMETHODIMP
BackgroundFileSaverStreamListener::OnStartRequest(nsIRequest* aRequest) {
  NS_ENSURE_ARG(aRequest);

  return NS_OK;
}

NS_IMETHODIMP
BackgroundFileSaverStreamListener::OnStopRequest(nsIRequest* aRequest,
                                                 nsresult aStatusCode) {
  if (NS_FAILED(aStatusCode)) {
    Finish(aStatusCode);
  }

  return NS_OK;
}

NS_IMETHODIMP
BackgroundFileSaverStreamListener::OnDataAvailable(nsIRequest* aRequest,
                                                   nsIInputStream* aInputStream,
                                                   uint64_t aOffset,
                                                   uint32_t aCount) {
  nsresult rv;

  NS_ENSURE_ARG(aRequest);

  uint32_t writeCount;
  rv = mPipeOutputStream->WriteFrom(aInputStream, aCount, &writeCount);
  NS_ENSURE_SUCCESS(rv, rv);

  if (writeCount < aCount) {
    NS_WARNING("Reading from the input stream should not have failed.");
    return NS_ERROR_UNEXPECTED;
  }

  bool stateChanged = false;
  {
    MutexAutoLock lock(mSuspensionLock);

    if (!mReceivedTooMuchData) {
      uint64_t available;
      nsresult rv = mPipeInputStream->Available(&available);
      if (NS_SUCCEEDED(rv) && available > REQUEST_SUSPEND_AT) {
        mReceivedTooMuchData = true;
        mRequest = aRequest;
        stateChanged = true;
      }
    }
  }

  if (stateChanged) {
    NotifySuspendOrResume();
  }

  return NS_OK;
}

void BackgroundFileSaverStreamListener::AsyncCopyProgressCallback(
    void* aClosure, uint32_t aCount) {
  BackgroundFileSaverStreamListener* self =
      (BackgroundFileSaverStreamListener*)aClosure;

  MutexAutoLock lock(self->mSuspensionLock);

  if (self->mReceivedTooMuchData) {
    uint64_t available;
    nsresult rv = self->mPipeInputStream->Available(&available);
    if (NS_FAILED(rv) || available < REQUEST_RESUME_AT) {
      self->mReceivedTooMuchData = false;

      if (NS_FAILED(self->mControlEventTarget->Dispatch(
              NewRunnableMethod(
                  "BackgroundFileSaverStreamListener::NotifySuspendOrResume",
                  self,
                  &BackgroundFileSaverStreamListener::NotifySuspendOrResume),
              NS_DISPATCH_NORMAL))) {
        NS_WARNING("Unable to post resume event to the control thread.");
      }
    }
  }
}

nsresult BackgroundFileSaverStreamListener::NotifySuspendOrResume() {
  MutexAutoLock lock(mSuspensionLock);

  if (mReceivedTooMuchData) {
    if (!mRequestSuspended) {
      if (NS_SUCCEEDED(mRequest->Suspend())) {
        mRequestSuspended = true;
      } else {
        NS_WARNING("Unable to suspend the request.");
      }
    }
  } else {
    if (mRequestSuspended) {
      if (NS_SUCCEEDED(mRequest->Resume())) {
        mRequestSuspended = false;
      } else {
        NS_WARNING("Unable to resume the request.");
      }
    }
  }

  return NS_OK;
}

}  
}  
