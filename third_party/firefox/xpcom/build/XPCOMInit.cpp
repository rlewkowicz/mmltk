/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ThreadEventTarget.h"
#include "XPCOMModule.h"

#include "base/basictypes.h"

#include "mozilla/AbstractThread.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/Assertions.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/SharedThreadPool.h"
#include "mozilla/TaskController.h"
#include "mozJSModuleLoader.h"
#include "nsXULAppAPI.h"

#  include "nsTerminator.h"

#include "nsXPCOMPrivate.h"
#include "nsXPCOMCIDInternal.h"

#include "mozilla/dom/JSExecutionManager.h"
#include "mozilla/dom/SharedScriptCache.h"
#include "mozilla/SharedStyleSheetCache.h"
#include "mozilla/layers/ImageBridgeChild.h"
#include "mozilla/layers/CompositorBridgeParent.h"

#include "prlink.h"

#include "nsCycleCollector.h"
#include "nsObserverService.h"

#include "nsDebugImpl.h"
#include "nsSystemInfo.h"

#include "nsComponentManager.h"
#include "nsCategoryManagerUtils.h"
#include "nsIServiceManager.h"

#include "nsThreadManager.h"
#include "nsThreadPool.h"

#include "nsTimerImpl.h"
#include "TimerThread.h"

#include "nsThread.h"
#include "nsVersionComparatorImpl.h"

#include "nsIFile.h"
#include "nsLocalFile.h"
#include "nsDirectoryService.h"
#include "nsDirectoryServiceDefs.h"
#include "nsCategoryManager.h"
#include "nsMultiplexInputStream.h"

#include "nsAtomTable.h"
#include "nsISupportsImpl.h"
#include "nsLanguageAtomService.h"

#include "nsSystemInfo.h"
#include "nsMemoryReporterManager.h"
#include "nss.h"
#include "nsNSSComponent.h"

#include <locale.h>
#include "mozilla/Services.h"
#include "mozilla/Omnijar.h"
#include "mozilla/ScriptPreloader.h"

#include "mozilla/PoisonIOInterposer.h"
#include "mozilla/scache/StartupCache.h"

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/message_loop.h"

#include "mozilla/ipc/IOThread.h"
#include "mozilla/AvailableMemoryTracker.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/CountingAllocatorBase.h"
#include "mozilla/ServoStyleConsts.h"

#include "mozilla/ipc/GeckoChildProcessHost.h"

#include "jsapi.h"
#include "js/Initialization.h"
#include "XPCSelfHostedShmem.h"

#include "gfxPlatform.h"

using base::AtExitManager;
using mozilla::ipc::IOThreadParent;

extern "C" void GkRust_Init();
extern "C" void GkRust_Shutdown();

namespace {

static AtExitManager* sExitManager;
static MessageLoop* sMessageLoop;
static bool sCommandLineWasInitialized;

} 


nsresult nsThreadManagerGetSingleton(const nsIID& aIID, void** aInstancePtr) {
  NS_ASSERTION(aInstancePtr, "null outptr");
  return nsThreadManager::get().QueryInterface(aIID, aInstancePtr);
}

nsresult nsLocalFileConstructor(const nsIID& aIID, void** aInstancePtr) {
  return nsLocalFile::nsLocalFileConstructor(aIID, aInstancePtr);
}

nsComponentManagerImpl* nsComponentManagerImpl::gComponentManager = nullptr;
bool gXPCOMShuttingDown = false;
bool gXPCOMMainThreadEventsAreDoomed = false;
char16_t* gGREBinPath = nullptr;

static nsIDebug2* gDebug = nullptr;

EXPORT_XPCOM_API(nsresult)
NS_GetDebug(nsIDebug2** aResult) {
  return nsDebugImpl::Create(NS_GET_IID(nsIDebug2), (void**)aResult);
}

class ICUReporter final : public nsIMemoryReporter,
                          public mozilla::CountingAllocatorBase<ICUReporter> {
 public:
  NS_DECL_ISUPPORTS

  static void* Alloc(const void*, size_t aSize) {
    void* result = CountingMalloc(aSize);
    if (result == nullptr) {
      MOZ_CRASH("Ran out of memory while allocating for ICU");
    }
    return result;
  }

  static void* Realloc(const void*, void* aPtr, size_t aSize) {
    void* result = CountingRealloc(aPtr, aSize);
    if (result == nullptr) {
      MOZ_CRASH("Ran out of memory while reallocating for ICU");
    }
    return result;
  }

  static void Free(const void*, void* aPtr) { return CountingFree(aPtr); }

 private:
  NS_IMETHOD
  CollectReports(nsIHandleReportCallback* aHandleReport, nsISupports* aData,
                 bool aAnonymize) override {
    MOZ_COLLECT_REPORT(
        "explicit/icu", KIND_HEAP, UNITS_BYTES, MemoryAllocated(),
        "Memory used by ICU, a Unicode and globalization support library.");

    return NS_OK;
  }

  ~ICUReporter() = default;
};

NS_IMPL_ISUPPORTS(ICUReporter, nsIMemoryReporter)

#define XPCOM_INIT_FATAL(message, res) \
  if (XRE_IsParentProcess()) {         \
    return res;                        \
  }                                    \
  MOZ_CRASH(message);

EXPORT_XPCOM_API(nsresult)
NS_InitXPCOM(nsIServiceManager** aResult, nsIFile* aBinDirectory,
             nsIDirectoryServiceProvider* aAppFileLocationProvider,
             bool aInitJSContext) {
  static bool sInitialized = false;
  if (sInitialized) {
    XPCOM_INIT_FATAL("!sInitialized", NS_ERROR_FAILURE)
  }

  sInitialized = true;

  NS_LogInit();

  NS_InitAtomTable();

  mozilla::LogModule::Init(0, nullptr);

  GkRust_Init();

  nsresult rv = NS_OK;

  gXPCOMShuttingDown = false;

#if defined(XP_UNIX)
  nsSystemInfo::gUserUmask = ::umask(0777);
  ::umask(nsSystemInfo::gUserUmask);
#endif

  NS_ASSERTION(!sExitManager && !sMessageLoop, "Bad logic!");

  if (!AtExitManager::AlreadyRegistered()) {
    sExitManager = new AtExitManager();
  }

  MessageLoop* messageLoop = MessageLoop::current();
  if (!messageLoop) {
    sMessageLoop = new MessageLoopForUI(MessageLoop::TYPE_MOZILLA_PARENT);
    sMessageLoop->set_thread_name("Gecko");
    sMessageLoop->set_hang_timeouts(128, 8192);
  } else if (messageLoop->type() == MessageLoop::TYPE_MOZILLA_CHILD) {
    messageLoop->set_thread_name("Gecko_Child");
    messageLoop->set_hang_timeouts(128, 8192);
  }

  rv = nsThreadManager::get().Init();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    XPCOM_INIT_FATAL("nsThreadManager::get().Init()", rv)
  }


  rv = nsTimerImpl::Startup();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    XPCOM_INIT_FATAL("nsTimerImpl::Startup()", rv)
  }

  if (strcmp(setlocale(LC_ALL, nullptr), "C") == 0) {
    setlocale(LC_ALL, "");
  }

  nsDirectoryService::RealInit();

  bool value;

  if (aBinDirectory) {
    rv = aBinDirectory->IsDirectory(&value);

    if (NS_SUCCEEDED(rv) && value) {
      nsDirectoryService::gService->SetCurrentProcessDirectory(aBinDirectory);
    }
  }

  if (aAppFileLocationProvider) {
    rv = nsDirectoryService::gService->RegisterProvider(
        aAppFileLocationProvider);
    if (NS_FAILED(rv)) {
      XPCOM_INIT_FATAL("nsDirectoryService::gService->RegisterProvider()", rv)
    }
  }

  nsCOMPtr<nsIFile> xpcomLib;
  nsDirectoryService::gService->Get(NS_GRE_BIN_DIR, NS_GET_IID(nsIFile),
                                    getter_AddRefs(xpcomLib));
  MOZ_ASSERT(xpcomLib);

  nsAutoString path;
  xpcomLib->GetPath(path);
  gGREBinPath = ToNewUnicode(path);

  xpcomLib->AppendNative(nsDependentCString(XPCOM_DLL));
  nsDirectoryService::gService->Set(NS_XPCOM_LIBRARY_FILE, xpcomLib);

  if (!mozilla::Omnijar::IsInitialized()) {
    MOZ_ASSERT(XRE_IsParentProcess() || XRE_IsContentProcess());

    nsresult rv = mozilla::Omnijar::FallibleInit();
    if (NS_FAILED(rv)) {
      XPCOM_INIT_FATAL("Omnijar::Init()", NS_ERROR_OMNIJAR_CORRUPT)
    }
  }

  if ((sCommandLineWasInitialized = !CommandLine::IsInitialized())) {
    nsCOMPtr<nsIFile> binaryFile;
    nsDirectoryService::gService->Get(NS_XPCOM_CURRENT_PROCESS_DIR,
                                      NS_GET_IID(nsIFile),
                                      getter_AddRefs(binaryFile));
    if (NS_WARN_IF(!binaryFile)) {
      XPCOM_INIT_FATAL("!binaryFile", NS_ERROR_FAILURE)
    }

    rv = binaryFile->AppendNative("nonexistent-executable"_ns);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      XPCOM_INIT_FATAL("binaryFile->AppendNative()", rv)
    }

    nsCString binaryPath;
    rv = binaryFile->GetNativePath(binaryPath);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      XPCOM_INIT_FATAL("binaryFile->GetNativePath", rv)
    }

    static char const* const argv = {strdup(binaryPath.get())};
    CommandLine::Init(1, &argv);
  }

  NS_ASSERTION(nsComponentManagerImpl::gComponentManager == nullptr,
               "CompMgr not null at init");

  nsComponentManagerImpl::gComponentManager = new nsComponentManagerImpl();
  NS_ADDREF(nsComponentManagerImpl::gComponentManager);

  if (!nsCycleCollector_init()) {
    XPCOM_INIT_FATAL("nsCycleCollector_init()", NS_ERROR_UNEXPECTED)
  }

  nsCycleCollector_startup();

  mozilla::SetICUMemoryFunctions();

  rv = nsComponentManagerImpl::gComponentManager->Init();
  if (NS_FAILED(rv)) {
    NS_RELEASE(nsComponentManagerImpl::gComponentManager);
    XPCOM_INIT_FATAL("gComponentManager->Init()", rv)
  }

  if (aResult) {
    NS_ADDREF(*aResult = nsComponentManagerImpl::gComponentManager);
  }


#if defined(MOZ_MEMORY)
  mozilla::TaskController::SetupIdleMemoryCleanup();
#endif

  nsDirectoryService::gService->RegisterCategoryProviders();

  mozilla::SharedThreadPool::InitStatics();

  mozilla::scache::StartupCache::GetSingleton();
  mozilla::AvailableMemoryTracker::Init();

  NS_CreateServicesFromCategory(NS_XPCOM_STARTUP_CATEGORY, nullptr,
                                NS_XPCOM_STARTUP_OBSERVER_ID);

  RegisterStrongMemoryReporter(mozilla::MakeAndAddRef<ICUReporter>());
  xpc::SelfHostedShmem::GetSingleton().InitMemoryReporter();

  mozilla::dom::JSExecutionManager::Initialize();

  if (aInitJSContext) {
    xpc::InitializeJSContext();
  }

  return NS_OK;
}

#undef XPCOM_INIT_FATAL

EXPORT_XPCOM_API(nsresult)
NS_InitMinimalXPCOM() {
  NS_SetMainThread();
  mozilla::TimeStamp::Startup();
  NS_LogInit();
  NS_InitAtomTable();

  mozilla::LogModule::Init(0, nullptr);

  GkRust_Init();

  nsresult rv = nsThreadManager::get().Init();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = nsTimerImpl::Startup();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsComponentManagerImpl::gComponentManager = new nsComponentManagerImpl();
  NS_ADDREF(nsComponentManagerImpl::gComponentManager);

  rv = nsComponentManagerImpl::gComponentManager->Init();
  if (NS_FAILED(rv)) {
    NS_RELEASE(nsComponentManagerImpl::gComponentManager);
    return rv;
  }

  if (!nsCycleCollector_init()) {
    return NS_ERROR_UNEXPECTED;
  }

  mozilla::SharedThreadPool::InitStatics();
  return NS_OK;
}

EXPORT_XPCOM_API(nsresult)
NS_ShutdownXPCOM(nsIServiceManager* aServMgr) {
  return mozilla::ShutdownXPCOM(aServMgr);
}

namespace mozilla {

void SetICUMemoryFunctions() {
  static bool sICUReporterInitialized = false;
  if (!sICUReporterInitialized) {
    if (!JS_SetICUMemoryFunctions(ICUReporter::Alloc, ICUReporter::Realloc,
                                  ICUReporter::Free)) {
      MOZ_CRASH("JS_SetICUMemoryFunctions failed.");
    }
    sICUReporterInitialized = true;
  }
}

nsresult ShutdownXPCOM(nsIServiceManager* aServMgr) {
  if (!NS_IsMainThread()) {
    MOZ_CRASH("Shutdown on wrong thread");
  }

  {

    nsCOMPtr<nsIThread> thread = do_GetCurrentThread();
    if (NS_WARN_IF(!thread)) {
      return NS_ERROR_UNEXPECTED;
    }

    mozilla::AppShutdown::AdvanceShutdownPhase(
        mozilla::ShutdownPhase::XPCOMWillShutdown);

    nsCOMPtr<nsIServiceManager> mgr;
    (void)NS_GetServiceManager(getter_AddRefs(mgr));
    MOZ_DIAGNOSTIC_ASSERT(mgr != nullptr, "Service manager not present!");
    mozilla::AppShutdown::AdvanceShutdownPhase(
        mozilla::ShutdownPhase::XPCOMShutdown, nullptr, do_QueryInterface(mgr));

    gfxPlatform::ShutdownLayersIPC();

    mozilla::AppShutdown::AdvanceShutdownPhase(
        mozilla::ShutdownPhase::XPCOMShutdownThreads);
#if defined(DEBUG)
    ThreadEventTarget::XPCOMShutdownThreadsNotificationFinished();
#endif

    nsTimerImpl::Shutdown();

    NS_ProcessPendingEvents(thread);

    nsThreadManager::get().ShutdownNonMainThreads();

    RefPtr<nsObserverService> observerService;
    CallGetService("@mozilla.org/observer-service;1",
                   (nsObserverService**)getter_AddRefs(observerService));
    if (observerService) {
      observerService->Shutdown();
    }

#if defined(NS_FREE_PERMANENT_DATA)
    Servo_ShutdownThreadPool();
#endif

    AppShutdown::AdvanceShutdownPhase(ShutdownPhase::XPCOMShutdownFinal);

    nsThreadManager::get().ShutdownMainThread();
    gXPCOMMainThreadEventsAreDoomed = true;

    mozilla::dom::JSExecutionManager::Shutdown();
  }

  mozilla::services::Shutdown();

  NS_IF_RELEASE(aServMgr);

  if (nsComponentManagerImpl::gComponentManager) {
    nsComponentManagerImpl::gComponentManager->FreeServices();
  }

  nsThreadManager::get().ReleaseMainThread();
  AbstractThread::ShutdownMainThread();

  nsDirectoryService::gService = nullptr;

  free(gGREBinPath);
  gGREBinPath = nullptr;

  mozJSModuleLoader::UnloadLoaders();


  mozilla::dom::SharedScriptCache::PrepareForLastCC();

  bool shutdownCollect;
#if defined(NS_FREE_PERMANENT_DATA)
  shutdownCollect = true;
#else
  shutdownCollect = !!PR_GetEnv("MOZ_CC_RUN_DURING_SHUTDOWN");
#endif
  nsCycleCollector_shutdown(shutdownCollect);

  AppShutdown::AdvanceShutdownPhase(ShutdownPhase::CCPostLastCycleCollection);

  mozilla::scache::StartupCache::DeleteSingleton();
  mozilla::ScriptPreloader::DeleteSingleton();


  if (nsComponentManagerImpl::gComponentManager) {
    DebugOnly<nsresult> rv =
        (nsComponentManagerImpl::gComponentManager)->Shutdown();
    NS_ASSERTION(NS_SUCCEEDED(rv.value), "Component Manager shutdown failed.");
  } else {
    NS_WARNING("Component Manager was never created ...");
  }

  mozilla::ScriptPreloader::DeleteCacheDataSingleton();

  mozilla::dom::SharedScriptCache::DeleteSingleton();
  mozilla::SharedStyleSheetCache::DeleteSingleton();

  xpc::SelfHostedShmem::Shutdown();

  if (NSS_IsInitialized()) {
    nsNSSComponent::DoClearSSLExternalAndInternalSessionCache();
    if (NSS_Shutdown() != SECSuccess) {
#if defined(DEBUG) && !0
      if (!getenv("MOZ_IGNORE_NSS_SHUTDOWN_LEAKS") &&
          !getenv("XPCOM_MEM_BLOAT_LOG") && !getenv("XPCOM_MEM_LEAK_LOG") &&
          !getenv("XPCOM_MEM_REFCNT_LOG") && !getenv("XPCOM_MEM_COMPTR_LOG")) {
        MOZ_CRASH("NSS_Shutdown failed");
      } else {
#if defined(NS_BUILD_REFCNT_LOGGING)
        NS_LogCtor((void*)0x100, "NSSShutdownFailed", 100);
#endif
        NS_WARNING("NSS_Shutdown failed");
      }
#else
      NS_WARNING("NSS_Shutdown failed");
#endif
    }
  }

  if (nsComponentManagerImpl::gComponentManager) {
    nsrefcnt cnt;
    NS_RELEASE2(nsComponentManagerImpl::gComponentManager, cnt);
    NS_ASSERTION(cnt == 0, "Component Manager being held past XPCOM shutdown.");
  }
  nsComponentManagerImpl::gComponentManager = nullptr;
  nsCategoryManager::Destroy();

  nsLanguageAtomService::Shutdown();

  GkRust_Shutdown();

#if defined(NS_FREE_PERMANENT_DATA)
  NS_ShutdownAtomTable();
#endif

  NS_IF_RELEASE(gDebug);

  mozilla::ipc::IOThread::Shutdown();

  delete sMessageLoop;
  sMessageLoop = nullptr;

  mozilla::TaskController::Shutdown();

  if (sCommandLineWasInitialized) {
    CommandLine::Terminate();
    sCommandLineWasInitialized = false;
  }

  delete sExitManager;
  sExitManager = nullptr;

  Omnijar::CleanUp();

  NS_LogTerm();

  return NS_OK;
}

}  
