/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GMPProcessParent.h"

#include "GMPUtils.h"
#include "nsIRunnable.h"
#include <string>

#include "GMPLog.h"
#include "base/process_util.h"
#include "base/string_util.h"
#include "mozilla/GeckoArgs.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/ipc/ProcessChild.h"
#include "mozilla/ipc/ProcessUtils.h"
#include "nsFmtString.h"


using std::string;
using std::vector;

using mozilla::gmp::GMPProcessParent;
using mozilla::ipc::GeckoChildProcessHost;

namespace mozilla::gmp {



GMPProcessParent::GMPProcessParent(const std::string& aGMPPath)
    : GeckoChildProcessHost(GeckoProcessType_GMPlugin),
      mGMPPath(aGMPPath),
      mUseXpcom(StaticPrefs::media_gmp_use_minimal_xpcom())
{
  MOZ_COUNT_CTOR(GMPProcessParent);
}

GMPProcessParent::~GMPProcessParent() { MOZ_COUNT_DTOR(GMPProcessParent); }

bool GMPProcessParent::Launch(int32_t aTimeoutMs) {
  class PrefSerializerRunnable final : public Runnable {
   public:
    PrefSerializerRunnable()
        : Runnable("GMPProcessParent::PrefSerializerRunnable"),
          mMonitor("GMPProcessParent::PrefSerializerRunnable::mMonitor") {}

    NS_IMETHOD Run() override {
      auto prefSerializer = MakeUnique<ipc::SharedPreferenceSerializer>();
      bool success =
          prefSerializer->SerializeToSharedMemory(GeckoProcessType_GMPlugin,
                                                   ""_ns);

      MonitorAutoLock lock(mMonitor);
      MOZ_ASSERT(!mComplete);
      if (success) {
        mPrefSerializer = std::move(prefSerializer);
      }
      mComplete = true;
      lock.Notify();
      return NS_OK;
    }

    void Wait(int32_t aTimeoutMs,
              UniquePtr<ipc::SharedPreferenceSerializer>& aOut) {
      MonitorAutoLock lock(mMonitor);

      TimeDuration timeout = TimeDuration::FromMilliseconds(aTimeoutMs);
      while (!mComplete) {
        if (lock.Wait(timeout) == CVStatus::Timeout) {
          return;
        }
      }

      aOut = std::move(mPrefSerializer);
    }

   private:
    Monitor mMonitor;
    UniquePtr<ipc::SharedPreferenceSerializer> mPrefSerializer
        MOZ_GUARDED_BY(mMonitor);
    bool mComplete MOZ_GUARDED_BY(mMonitor) = false;
  };

  nsresult rv;
  geckoargs::ChildProcessArgs args;
  UniquePtr<ipc::SharedPreferenceSerializer> prefSerializer;

  ipc::ProcessChild::AddPlatformBuildID(args);

  if (mUseXpcom) {
    auto prefTask = MakeRefPtr<PrefSerializerRunnable>();
    rv = NS_DispatchToMainThread(prefTask);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return false;
    }

    prefTask->Wait(aTimeoutMs, prefSerializer);
    if (NS_WARN_IF(!prefSerializer)) {
      return false;
    }

    prefSerializer->AddSharedPrefCmdLineArgs(*this, args);
  }

  geckoargs::sPluginNativeEvent.Put(
      StaticPrefs::media_gmp_use_native_event_processing(), args);

#ifdef ALLOW_GECKO_CHILD_PROCESS_ARCH
  GMP_LOG_DEBUG("GMPProcessParent::Launch() mLaunchArch: {}", mLaunchArch);
#endif

  nsAutoCString normalizedPath;
  rv = NormalizePath(mGMPPath.c_str(), normalizedPath);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    GMP_LOG_DEBUG(
        "GMPProcessParent::Launch: "
        "plugin path normaliziation failed for path: {}",
        mGMPPath.c_str());
  }

  if (NS_SUCCEEDED(rv)) {
    geckoargs::sPluginPath.Put(normalizedPath.get(), args);
  } else {
    geckoargs::sPluginPath.Put(mGMPPath.c_str(), args);
  }

  bool launched = SyncLaunch(std::move(args), aTimeoutMs);
  if (launched) {
    nsFmtString name{u"GMPProcessParent {}", static_cast<void*>(this)};
    mShutdownBlocker = media::ShutdownBlockingTicket::Create(
        name, NS_LITERAL_STRING_FROM_CSTRING(__FILE__), __LINE__);
  }
  return launched;
}

void GMPProcessParent::Delete(nsCOMPtr<nsIRunnable> aCallback) {
  mDeletedCallback = std::move(aCallback);
  XRE_GetAsyncIOEventTarget()->Dispatch(NewNonOwningRunnableMethod(
      "gmp::GMPProcessParent::DoDelete", this, &GMPProcessParent::DoDelete));
}

void GMPProcessParent::DoDelete() {
  MOZ_ASSERT(XRE_GetAsyncIOEventTarget()->IsOnCurrentThread());

  if (mDeletedCallback) {
    mDeletedCallback->Run();
  }

  Destroy();
}


nsresult GMPProcessParent::NormalizePath(const char* aPath,
                                         PathString& aNormalizedPath) {
  nsCOMPtr<nsIFile> fileOrDir;
  nsresult rv =
      NS_NewLocalFile(NS_ConvertUTF8toUTF16(aPath), getter_AddRefs(fileOrDir));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = fileOrDir->Normalize();
  NS_ENSURE_SUCCESS(rv, rv);

  bool isLink = false;
  rv = fileOrDir->IsSymlink(&isLink);
  NS_ENSURE_SUCCESS(rv, rv);
  if (isLink) {
    return fileOrDir->GetNativeTarget(aNormalizedPath);
  }
  return fileOrDir->GetNativePath(aNormalizedPath);
}

}  
