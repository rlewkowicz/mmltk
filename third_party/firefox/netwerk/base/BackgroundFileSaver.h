/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef BackgroundFileSaver_h_
#define BackgroundFileSaver_h_

#include "mozilla/Mutex.h"
#include "nsCOMArray.h"
#include "nsCOMPtr.h"
#include "nsIAsyncOutputStream.h"
#include "nsIBackgroundFileSaver.h"
#include "nsIStreamListener.h"
#include "nsStreamUtils.h"
#include "nsString.h"

class nsIAsyncInputStream;
class nsISerialEventTarget;

namespace mozilla {
namespace net {


class BackgroundFileSaver : public nsIBackgroundFileSaver {
 public:
  NS_DECL_NSIBACKGROUNDFILESAVER

  BackgroundFileSaver();

  nsresult Init();

  static uint32_t sThreadCount;

  static uint32_t sTelemetryMaxThreadCount;

 protected:
  virtual ~BackgroundFileSaver();

  nsCOMPtr<nsIEventTarget> mControlEventTarget;

  nsCOMPtr<nsISerialEventTarget> mBackgroundET;

  nsCOMPtr<nsIAsyncOutputStream> mPipeOutputStream;

  virtual bool HasInfiniteBuffer() = 0;

  virtual nsAsyncCopyProgressFun GetProgressCallback() = 0;

  nsCOMPtr<nsIAsyncInputStream> mPipeInputStream;

 private:
  friend class NotifyTargetChangeRunnable;

  nsCOMPtr<nsIBackgroundFileSaverObserver> mObserver;


  mozilla::Mutex mLock{"BackgroundFileSaver.mLock"};

  bool mWorkerThreadAttentionRequested MOZ_GUARDED_BY(mLock){false};

  bool mFinishRequested MOZ_GUARDED_BY(mLock){false};

  bool mComplete MOZ_GUARDED_BY(mLock){false};

  nsresult mStatus MOZ_GUARDED_BY(mLock){NS_OK};

  bool mAppend MOZ_GUARDED_BY(mLock){false};

  nsCOMPtr<nsIFile> mInitialTarget MOZ_GUARDED_BY(mLock);

  bool mInitialTargetKeepPartial MOZ_GUARDED_BY(mLock){false};

  nsCOMPtr<nsIFile> mRenamedTarget MOZ_GUARDED_BY(mLock);

  bool mRenamedTargetKeepPartial MOZ_GUARDED_BY(mLock){false};

  nsCOMPtr<nsISupports> mAsyncCopyContext MOZ_GUARDED_BY(mLock);


  nsCOMPtr<nsIFile> mActualTarget;

  bool mActualTargetKeepPartial{false};


  static void AsyncCopyCallback(void* aClosure, nsresult aStatus);

  nsresult GetWorkerThreadAttention(bool aShouldInterruptCopy);

  nsresult ProcessAttention();

  nsresult ProcessStateChange();

  bool CheckCompletion();

  nsresult NotifyTargetChange(nsIFile* aTarget);

  nsresult NotifySaveComplete();

};


class BackgroundFileSaverOutputStream : public BackgroundFileSaver,
                                        public nsIAsyncOutputStream,
                                        public nsIOutputStreamCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIOUTPUTSTREAM
  NS_DECL_NSIASYNCOUTPUTSTREAM
  NS_DECL_NSIOUTPUTSTREAMCALLBACK

  BackgroundFileSaverOutputStream();

 protected:
  virtual bool HasInfiniteBuffer() override;
  virtual nsAsyncCopyProgressFun GetProgressCallback() override;

 private:
  ~BackgroundFileSaverOutputStream() = default;

  nsCOMPtr<nsIOutputStreamCallback> mAsyncWaitCallback;
};


class BackgroundFileSaverStreamListener final : public BackgroundFileSaver,
                                                public nsIStreamListener {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIREQUESTOBSERVER
  NS_DECL_NSISTREAMLISTENER

  BackgroundFileSaverStreamListener() = default;

 protected:
  virtual bool HasInfiniteBuffer() override;
  virtual nsAsyncCopyProgressFun GetProgressCallback() override;

 private:
  ~BackgroundFileSaverStreamListener() = default;

  mozilla::Mutex mSuspensionLock{
      "BackgroundFileSaverStreamListener.mSuspensionLock"};

  bool mReceivedTooMuchData MOZ_GUARDED_BY(mSuspensionLock){false};

  nsCOMPtr<nsIRequest> mRequest MOZ_GUARDED_BY(mSuspensionLock);

  bool mRequestSuspended MOZ_GUARDED_BY(mSuspensionLock){false};

  static void AsyncCopyProgressCallback(void* aClosure, uint32_t aCount);

  nsresult NotifySuspendOrResume();
};

}  
}  

#endif
