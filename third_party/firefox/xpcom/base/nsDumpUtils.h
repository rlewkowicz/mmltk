/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_nsDumpUtils_h)
#define mozilla_nsDumpUtils_h

#include "nsIObserver.h"
#include "base/message_loop.h"
#include "nsXULAppAPI.h"
#include "nsThreadUtils.h"
#include "mozilla/Mutex.h"
#include "mozilla/StaticPtr.h"
#include "nsTArray.h"

#if defined(LOG)
#  undef LOG
#endif

#  define LOG(...)

#if defined(XP_UNIX) && !0  // {

class FdWatcher : public MessageLoopForIO::Watcher, public nsIObserver {
 protected:
  MessageLoopForIO::FileDescriptorWatcher mReadWatcher;
  int mFd;

  virtual ~FdWatcher() {
    MOZ_ASSERT(mFd == -1);
  }

 public:
  FdWatcher() : mFd(-1) { MOZ_ASSERT(NS_IsMainThread()); }

  virtual int OpenFd() = 0;

  virtual void OnFileCanReadWithoutBlocking(int aFd) override = 0;
  virtual void OnFileCanWriteWithoutBlocking(int aFd) override {};

  NS_DECL_THREADSAFE_ISUPPORTS

  void Init();


  virtual void StartWatching();

  virtual void StopWatching();

  NS_IMETHOD Observe(nsISupports* aSubject, const char* aTopic,
                     const char16_t* aData) override {
    MOZ_ASSERT(NS_IsMainThread());
    MOZ_ASSERT(!strcmp(aTopic, "xpcom-shutdown"));

    XRE_GetAsyncIOEventTarget()->Dispatch(mozilla::NewRunnableMethod(
        "FdWatcher::StopWatching", this, &FdWatcher::StopWatching));

    return NS_OK;
  }
};

typedef void (*FifoCallback)(const nsCString& aInputStr);
struct FifoInfo {
  nsCString mCommand;
  FifoCallback mCallback;
};
typedef nsTArray<FifoInfo> FifoInfoArray;

class FifoWatcher : public FdWatcher {
 public:
  static const char kPrefName[38];

  static FifoWatcher* GetSingleton();

  static bool MaybeCreate();

  void RegisterCallback(const nsCString& aCommand, FifoCallback aCallback);

  virtual ~FifoWatcher();

  virtual int OpenFd() override;

  virtual void OnFileCanReadWithoutBlocking(int aFd) override;

 private:
  nsAutoCString mDirPath;

  static mozilla::StaticRefPtr<FifoWatcher> sSingleton;

  explicit FifoWatcher(nsCString aPath)
      : mDirPath(std::move(aPath)),
        mFifoInfoLock("FifoWatcher.mFifoInfoLock") {}

  mozilla::Mutex mFifoInfoLock;  
  FifoInfoArray mFifoInfo MOZ_GUARDED_BY(mFifoInfoLock);
};

typedef void (*PipeCallback)(const uint8_t aRecvSig);
struct SignalInfo {
  uint8_t mSignal;
  PipeCallback mCallback;
};
typedef nsTArray<SignalInfo> SignalInfoArray;

class SignalPipeWatcher : public FdWatcher {
 public:
  static SignalPipeWatcher* GetSingleton();

  void RegisterCallback(uint8_t aSignal, PipeCallback aCallback);

  void RegisterSignalHandler(uint8_t aSignal = 0);

  virtual ~SignalPipeWatcher();

  virtual int OpenFd() override;

  virtual void StopWatching() override;

  virtual void OnFileCanReadWithoutBlocking(int aFd) override;

 private:
  static mozilla::StaticRefPtr<SignalPipeWatcher> sSingleton;

  SignalPipeWatcher() : mSignalInfoLock("SignalPipeWatcher.mSignalInfoLock") {
    MOZ_ASSERT(NS_IsMainThread());
  }

  mozilla::Mutex mSignalInfoLock;  
  SignalInfoArray mSignalInfo MOZ_GUARDED_BY(mSignalInfoLock);
};

#endif

class nsDumpUtils {
 public:
  enum Mode { CREATE, CREATE_UNIQUE };

  static nsresult OpenTempFile(const nsACString& aFilename, nsIFile** aFile,
                               const nsACString& aFoldername = ""_ns,
                               Mode aMode = CREATE_UNIQUE);
};

#endif
