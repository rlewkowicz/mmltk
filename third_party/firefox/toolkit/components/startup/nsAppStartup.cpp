/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsAppStartup.h"

#include "nsComponentManagerUtils.h"
#include "nsIAppShellService.h"
#include "nsPIDOMWindow.h"
#include "nsIInterfaceRequestor.h"
#include "nsIFile.h"
#include "nsIObserverService.h"
#include "nsIPrefBranch.h"
#include "nsIPrefService.h"
#include "nsIProcess.h"
#include "nsIToolkitProfile.h"
#include "nsIWebBrowserChrome.h"
#include "nsIWindowMediator.h"
#include "nsIXULRuntime.h"
#include "nsIAppWindow.h"
#include "nsNativeCharsetUtils.h"
#include "nsThreadUtils.h"
#include "nsString.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/Preferences.h"
#include "mozilla/ResultExtensions.h"
#include "mozilla/Try.h"
#include "prprf.h"
#include "nsIInterfaceRequestorUtils.h"
#include "nsWidgetsCID.h"
#include "nsAppRunner.h"
#include "nsAppShellCID.h"
#include "nsXPCOMCIDInternal.h"
#include "mozilla/Services.h"
#include "jsapi.h"
#include "js/Date.h"
#include "js/PropertyAndElement.h"  // JS_DefineProperty
#include "prenv.h"
#include "nsAppDirectoryServiceDefs.h"


#include "mozilla/IOInterposer.h"
#include "mozilla/StartupTimeline.h"

static NS_DEFINE_CID(kAppShellCID, NS_APPSHELL_CID);

#define kPrefLastSuccess "toolkit.startup.last_success"
#define kPrefMaxResumedCrashes "toolkit.startup.max_resumed_crashes"
#define kPrefRecentCrashes "toolkit.startup.recent_crashes"
#define kPrefAlwaysUseSafeMode "toolkit.startup.always_use_safe_mode"

#define kNanosecondsPerSecond 1000000000.0


using namespace mozilla;

class nsAppExitEvent : public mozilla::Runnable {
 private:
  RefPtr<nsAppStartup> mService;

 public:
  explicit nsAppExitEvent(nsAppStartup* service)
      : mozilla::Runnable("nsAppExitEvent"), mService(service) {}

  NS_IMETHOD Run() override {
    mService->mAppShell->Exit();

    mService->mRunning = false;
    return NS_OK;
  }
};

static uint64_t ComputeAbsoluteTimestamp(TimeStamp stamp) {
  static PRTime sAbsoluteNow = PR_Now();
  static TimeStamp sMonotonicNow = TimeStamp::Now();

  return sAbsoluteNow - (sMonotonicNow - stamp).ToMicroseconds();
}


nsAppStartup::nsAppStartup()
    : mConsiderQuitStopper(0),
      mRunning(false),
      mShuttingDown(false),
      mStartingUp(true),
      mAttemptingQuit(false),
      mIsSafeModeNecessary(false),
      mStartupCrashAndHangTrackingEnded(false) {
  char* mozAppSilentStart = PR_GetEnv("MOZ_APP_SILENT_START");

  mWasSilentlyStarted =
      mozAppSilentStart && (strcmp(mozAppSilentStart, "") != 0);

  PR_SetEnv("MOZ_APP_SILENT_START=");

}

nsAppStartup::~nsAppStartup() = default;

nsresult nsAppStartup::Init() {
  nsresult rv;

  mAppShell = do_GetService(kAppShellCID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService();
  if (!os) return NS_ERROR_FAILURE;

  os->AddObserver(this, "quit-application", true);
  os->AddObserver(this, "quit-application-forced", true);
  os->AddObserver(this, "sessionstore-init-started", true);
  os->AddObserver(this, "sessionstore-windows-restored", true);
  os->AddObserver(this, "profile-change-teardown", true);
  os->AddObserver(this, "xul-window-registered", true);
  os->AddObserver(this, "xul-window-destroyed", true);
  os->AddObserver(this, "profile-before-change", true);
  os->AddObserver(this, "xpcom-shutdown", true);


  return NS_OK;
}


NS_IMPL_ISUPPORTS(nsAppStartup, nsIAppStartup, nsIWindowCreator, nsIObserver,
                  nsISupportsWeakReference)


NS_IMETHODIMP
nsAppStartup::CreateHiddenWindow() {
  nsCOMPtr<nsIAppShellService> appShellService(
      do_GetService(NS_APPSHELLSERVICE_CONTRACTID));
  NS_ENSURE_TRUE(appShellService, NS_ERROR_FAILURE);

  return appShellService->CreateHiddenWindow();
}

NS_IMETHODIMP
nsAppStartup::DestroyHiddenWindow() {
  nsCOMPtr<nsIAppShellService> appShellService(
      do_GetService(NS_APPSHELLSERVICE_CONTRACTID));
  NS_ENSURE_TRUE(appShellService, NS_ERROR_FAILURE);

  return appShellService->DestroyHiddenWindow();
}

NS_IMETHODIMP
nsAppStartup::Run(void) {
  NS_ASSERTION(!mRunning, "Reentrant appstartup->Run()");


  if (!mShuttingDown && mConsiderQuitStopper != 0) {

    mRunning = true;

    nsresult rv = mAppShell->Run();
    if (NS_FAILED(rv)) return rv;
  }

  bool userAllowedQuit = true;
  Quit(eForceQuit, 0, &userAllowedQuit);

  nsresult retval = NS_OK;
  if (mozilla::AppShutdown::IsRestarting()) {
    retval = NS_SUCCESS_RESTART_APP;
  }

  return retval;
}

NS_IMETHODIMP
nsAppStartup::Quit(uint32_t aMode, int aExitCode, bool* aUserAllowedQuit) {
  if ((aMode & eSilently) != 0 && (aMode & eRestart) == 0) {
    return NS_ERROR_INVALID_ARG;
  }

  uint32_t ferocity = (aMode & 0xF);

  *aUserAllowedQuit = false;

  nsresult rv = NS_OK;
  bool postedExitEvent = false;

  if (mShuttingDown) return NS_OK;

  if (ferocity == eConsiderQuit) {

    if (mConsiderQuitStopper == 0) {
      ferocity = eAttemptQuit;
    }
  }

  nsCOMPtr<nsIObserverService> obsService;
  if (ferocity == eAttemptQuit || ferocity == eForceQuit) {
    nsCOMPtr<nsISimpleEnumerator> windowEnumerator;
    nsCOMPtr<nsIWindowMediator> mediator(
        do_GetService(NS_WINDOWMEDIATOR_CONTRACTID));
    if (mediator) {
      mediator->GetEnumerator(nullptr, getter_AddRefs(windowEnumerator));
      if (windowEnumerator) {
        bool more;
        windowEnumerator->HasMoreElements(&more);
        MOZ_ASSERT_IF(!mConsiderQuitStopper, !more);

        while (more) {
          nsCOMPtr<nsISupports> window;
          windowEnumerator->GetNext(getter_AddRefs(window));
          nsCOMPtr<nsPIDOMWindowOuter> domWindow(do_QueryInterface(window));
          if (domWindow) {
            if (!domWindow->CanClose()) {
              return NS_OK;
            }
          }
          windowEnumerator->HasMoreElements(&more);
        }
      }
    }



    *aUserAllowedQuit = true;
    mShuttingDown = true;
    auto shutdownMode = ((aMode & eRestart) != 0)
                            ? mozilla::AppShutdownMode::Restart
                            : mozilla::AppShutdownMode::Normal;
    auto shutdownReason = ((aMode & eRestart) != 0)
                              ? mozilla::AppShutdownReason::AppRestart
                              : mozilla::AppShutdownReason::AppClose;
    mozilla::AppShutdown::Init(shutdownMode, aExitCode, shutdownReason);

    if (mozilla::AppShutdown::IsRestarting()) {
      PR_SetEnv("MOZ_APP_RESTART=1");

      TimeStamp::RecordProcessRestart();
    }

    if ((aMode & eSilently) != 0) {
      PR_SetEnv("MOZ_APP_SILENT_START=1");
    }

    obsService = mozilla::services::GetObserverService();

    if (!mAttemptingQuit) {
      mAttemptingQuit = true;
      if (obsService)
        obsService->NotifyObservers(nullptr, "quit-application-granted",
                                    nullptr);
    }

    CloseAllWindows();

    if (mediator) {
      if (ferocity == eAttemptQuit) {
        ferocity = eForceQuit;  


        mediator->GetEnumerator(nullptr, getter_AddRefs(windowEnumerator));
        if (windowEnumerator) {
          bool more;
          while (NS_SUCCEEDED(windowEnumerator->HasMoreElements(&more)) &&
                 more) {
            ferocity = eAttemptQuit;
            nsCOMPtr<nsISupports> window;
            windowEnumerator->GetNext(getter_AddRefs(window));
            nsCOMPtr<nsPIDOMWindowOuter> domWindow = do_QueryInterface(window);
            if (domWindow) {
              if (!domWindow->Closed()) {
                MOZ_DIAGNOSTIC_ASSERT(false,
                                      "CloseAllWindows() did not succeed.");
                rv = NS_ERROR_FAILURE;
                break;
              }
            }
          }
        }
      }
    }
  }

  if (ferocity == eForceQuit) {
    mozilla::AppShutdown::OnShutdownConfirmed();

    bool isRestarting = mozilla::AppShutdown::IsRestarting();
    mozilla::AppShutdown::AdvanceShutdownPhase(
        mozilla::ShutdownPhase::AppShutdownConfirmed,
        isRestarting ? u"restart" : u"shutdown");

    if (!mRunning) {
      postedExitEvent = true;
    } else {
      nsCOMPtr<nsIRunnable> event = new nsAppExitEvent(this);
      rv = NS_DispatchToCurrentThread(event);
      if (NS_SUCCEEDED(rv)) {
        postedExitEvent = true;
      } else {
        NS_WARNING("failed to dispatch nsAppExitEvent");
      }
    }
  }

  if (!postedExitEvent) {
    mShuttingDown = false;
  }
  return rv;
}

static_assert(int(nsIAppStartup::SHUTDOWN_PHASE_NOTINSHUTDOWN) ==
                      int(mozilla::ShutdownPhase::NotInShutdown) &&
                  int(nsIAppStartup::SHUTDOWN_PHASE_APPSHUTDOWNCONFIRMED) ==
                      int(mozilla::ShutdownPhase::AppShutdownConfirmed) &&
                  int(nsIAppStartup::SHUTDOWN_PHASE_APPSHUTDOWNNETTEARDOWN) ==
                      int(mozilla::ShutdownPhase::AppShutdownNetTeardown) &&
                  int(nsIAppStartup::SHUTDOWN_PHASE_APPSHUTDOWNTEARDOWN) ==
                      int(mozilla::ShutdownPhase::AppShutdownTeardown) &&
                  int(nsIAppStartup::SHUTDOWN_PHASE_APPSHUTDOWN) ==
                      int(mozilla::ShutdownPhase::AppShutdown) &&
                  int(nsIAppStartup::SHUTDOWN_PHASE_APPSHUTDOWNQM) ==
                      int(mozilla::ShutdownPhase::AppShutdownQM) &&
                  int(nsIAppStartup::SHUTDOWN_PHASE_APPSHUTDOWNRELEMETRY) ==
                      int(mozilla::ShutdownPhase::AppShutdownTelemetry) &&
                  int(nsIAppStartup::SHUTDOWN_PHASE_XPCOMWILLSHUTDOWN) ==
                      int(mozilla::ShutdownPhase::XPCOMWillShutdown) &&
                  int(nsIAppStartup::SHUTDOWN_PHASE_XPCOMSHUTDOWN) ==
                      int(mozilla::ShutdownPhase::XPCOMShutdown),
              "IDLShutdownPhase values are as expected");

Result<ShutdownPhase, nsresult> IDLShutdownPhaseToNative(
    nsAppStartup::IDLShutdownPhase aPhase) {
  if (uint8_t(aPhase) <= nsIAppStartup::SHUTDOWN_PHASE_XPCOMSHUTDOWN) {
    return ShutdownPhase(aPhase);
  }
  return Err(NS_ERROR_ILLEGAL_VALUE);
}

NS_IMETHODIMP
nsAppStartup::AdvanceShutdownPhase(IDLShutdownPhase aPhase) {
  ShutdownPhase nativePhase = MOZ_TRY(IDLShutdownPhaseToNative(aPhase));
  AppShutdown::AdvanceShutdownPhase(nativePhase);
  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::IsInOrBeyondShutdownPhase(IDLShutdownPhase aPhase,
                                        bool* aIsInOrBeyond) {
  ShutdownPhase nativePhase = MOZ_TRY(IDLShutdownPhaseToNative(aPhase));
  *aIsInOrBeyond = AppShutdown::IsInOrBeyond(nativePhase);
  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::SetImpendingShutdown() {
  AppShutdown::SetImpendingShutdown();
  return NS_OK;
}

void nsAppStartup::CloseAllWindows() {
  nsCOMPtr<nsIWindowMediator> mediator(
      do_GetService(NS_WINDOWMEDIATOR_CONTRACTID));

  nsCOMPtr<nsISimpleEnumerator> windowEnumerator;

  mediator->GetEnumerator(nullptr, getter_AddRefs(windowEnumerator));

  if (!windowEnumerator) return;

  bool more;
  while (NS_SUCCEEDED(windowEnumerator->HasMoreElements(&more)) && more) {
    nsCOMPtr<nsISupports> isupports;
    if (NS_FAILED(windowEnumerator->GetNext(getter_AddRefs(isupports)))) break;

    nsCOMPtr<nsPIDOMWindowOuter> window = do_QueryInterface(isupports);
    NS_ASSERTION(window, "not an nsPIDOMWindow");
    if (window) {
      window->ForceClose();
    }
  }
}

NS_IMETHODIMP
nsAppStartup::EnterLastWindowClosingSurvivalArea(void) {
  ++mConsiderQuitStopper;
  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::ExitLastWindowClosingSurvivalArea(void) {
  NS_ASSERTION(mConsiderQuitStopper > 0, "consider quit stopper out of bounds");
  --mConsiderQuitStopper;

  if (mRunning) {
    bool userAllowedQuit = false;

    Quit(eConsiderQuit, mozilla::AppShutdown::GetExitCode(), &userAllowedQuit);
  }

  return NS_OK;
}


NS_IMETHODIMP
nsAppStartup::GetShuttingDown(bool* aResult) {
  *aResult = AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed);
  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::GetAttemptingQuit(bool* aResult) {
  *aResult = mAttemptingQuit;
  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::GetStartingUp(bool* aResult) {
  *aResult = mStartingUp;
  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::DoneStartingUp() {
  MOZ_ASSERT(mStartingUp);

  mStartingUp = false;
  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::GetRestarting(bool* aResult) {
  *aResult = mozilla::AppShutdown::IsRestarting();
  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::GetWasRestarted(bool* aResult) {
  char* mozAppRestart = PR_GetEnv("MOZ_APP_RESTART");

  *aResult = mozAppRestart && (strcmp(mozAppRestart, "") != 0);

  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::GetWasSilentlyStarted(bool* aResult) {
  *aResult = mWasSilentlyStarted;
  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::GetSecondsSinceLastOSRestart(int64_t* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsAppStartup::GetShowedPreXULSkeletonUI(bool* aResult) {
  *aResult = false;
  return NS_OK;
}


NS_IMETHODIMP
nsAppStartup::CreateChromeWindow(nsIWebBrowserChrome* aParent,
                                 uint32_t aChromeFlags,
                                 nsIOpenWindowInfo* aOpenWindowInfo,
                                 bool* aCancel, nsIWebBrowserChrome** _retval) {
  NS_ENSURE_ARG_POINTER(aCancel);
  NS_ENSURE_ARG_POINTER(_retval);
  *aCancel = false;
  *_retval = nullptr;

  if (mAttemptingQuit &&
      (aChromeFlags & nsIWebBrowserChrome::CHROME_MODAL) == 0)
    return NS_ERROR_ILLEGAL_DURING_SHUTDOWN;

  if ((aChromeFlags & nsIWebBrowserChrome::CHROME_FISSION_WINDOW) &&
      !(aChromeFlags & nsIWebBrowserChrome::CHROME_REMOTE_WINDOW)) {
    NS_WARNING("Cannot create non-remote fission window!");
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIAppWindow> newWindow;

  if (aParent) {
    nsCOMPtr<nsIAppWindow> appParent(do_GetInterface(aParent));
    NS_ASSERTION(appParent,
                 "window created using non-app parent. that's unexpected, but "
                 "may work.");

    if (appParent)
      appParent->CreateNewWindow(aChromeFlags, aOpenWindowInfo,
                                 getter_AddRefs(newWindow));
  } else {  
    MOZ_RELEASE_ASSERT(!aOpenWindowInfo,
                       "Unexpected aOpenWindowInfo, we shouldn't ever have an "
                       "nsIOpenWindowInfo without a parent");

    if (aChromeFlags & nsIWebBrowserChrome::CHROME_DEPENDENT)
      NS_WARNING("dependent window created without a parent");

    nsCOMPtr<nsIAppShellService> appShell(
        do_GetService(NS_APPSHELLSERVICE_CONTRACTID));
    if (!appShell) return NS_ERROR_FAILURE;

    appShell->CreateTopLevelWindow(
        nullptr, nullptr, aChromeFlags, nsIAppShellService::SIZE_TO_CONTENT,
        nsIAppShellService::SIZE_TO_CONTENT, getter_AddRefs(newWindow));
  }

  if (newWindow) {
    nsCOMPtr<nsIInterfaceRequestor> thing(do_QueryInterface(newWindow));
    if (thing) CallGetInterface(thing.get(), _retval);
  }

  return *_retval ? NS_OK : NS_ERROR_FAILURE;
}


NS_IMETHODIMP
nsAppStartup::Observe(nsISupports* aSubject, const char* aTopic,
                      const char16_t* aData) {
  NS_ASSERTION(mAppShell, "appshell service notified before appshell built");
  if (!strcmp(aTopic, "quit-application-forced")) {
    mShuttingDown = true;
  } else if (!strcmp(aTopic, "profile-change-teardown")) {
    if (!mShuttingDown) {
      EnterLastWindowClosingSurvivalArea();
      CloseAllWindows();
      ExitLastWindowClosingSurvivalArea();
    }
  } else if (!strcmp(aTopic, "xul-window-registered")) {
    EnterLastWindowClosingSurvivalArea();
  } else if (!strcmp(aTopic, "xul-window-destroyed")) {
    ExitLastWindowClosingSurvivalArea();
  } else if (!strcmp(aTopic, "sessionstore-windows-restored")) {
    StartupTimeline::Record(StartupTimeline::SESSION_RESTORED);
    IOInterposer::EnteringNextStage();
  } else if (!strcmp(aTopic, "sessionstore-init-started")) {
    StartupTimeline::Record(StartupTimeline::SESSION_RESTORE_INIT);
  } else if (!strcmp(aTopic, "xpcom-shutdown")) {
    IOInterposer::EnteringNextStage();
  } else if (!strcmp(aTopic, "quit-application")) {
    StartupTimeline::Record(StartupTimeline::QUIT_APPLICATION);
  } else if (!strcmp(aTopic, "profile-before-change")) {
    StartupTimeline::Record(StartupTimeline::PROFILE_BEFORE_CHANGE);
  } else {
    NS_ERROR("Unexpected observer topic.");
  }

  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::GetStartupInfo(JSContext* aCx,
                             JS::MutableHandle<JS::Value> aRetval) {
  JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));

  aRetval.setObject(*obj);

  TimeStamp procTime = StartupTimeline::Get(StartupTimeline::PROCESS_CREATION);

  if (procTime.IsNull()) {
    procTime = TimeStamp::ProcessCreation();

    StartupTimeline::Record(StartupTimeline::PROCESS_CREATION, procTime);
  }

  for (int i = StartupTimeline::PROCESS_CREATION;
       i < StartupTimeline::MAX_EVENT_ID; ++i) {
    StartupTimeline::Event ev = static_cast<StartupTimeline::Event>(i);
    TimeStamp stamp = StartupTimeline::Get(ev);

    if (stamp.IsNull() && (ev == StartupTimeline::MAIN)) {
      stamp = procTime;
      MOZ_ASSERT(!stamp.IsNull());
    }

    if (!stamp.IsNull()) {
      if (stamp >= procTime) {
        PRTime prStamp = ComputeAbsoluteTimestamp(stamp) / PR_USEC_PER_MSEC;
        JS::Rooted<JSObject*> date(
            aCx, JS::NewDateObject(aCx, JS::TimeClip(prStamp)));
        JS_DefineProperty(aCx, obj, StartupTimeline::Describe(ev), date,
                          JSPROP_ENUMERATE);
      }
    }
  }

  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::GetAutomaticSafeModeNecessary(bool* _retval) {
  NS_ENSURE_ARG_POINTER(_retval);

  bool alwaysSafe = false;
  Preferences::GetBool(kPrefAlwaysUseSafeMode, &alwaysSafe);

  if (!alwaysSafe) {
#if DEBUG
    mIsSafeModeNecessary = false;
#else
    mIsSafeModeNecessary &= !PR_GetEnv("MOZ_DISABLE_AUTO_SAFE_MODE");
#endif
  }

  *_retval = mIsSafeModeNecessary;
  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::TrackStartupCrashBegin(bool* aIsSafeModeNecessary) {
  const int32_t MAX_TIME_SINCE_STARTUP = 6 * 60 * 60 * 1000;
  const int32_t MAX_STARTUP_BUFFER = 10;
  nsresult rv;

  mStartupCrashAndHangTrackingEnded = false;

  StartupTimeline::Record(StartupTimeline::STARTUP_CRASH_DETECTION_BEGIN);

  bool hasLastSuccess = Preferences::HasUserValue(kPrefLastSuccess);
  if (!hasLastSuccess) {
    Preferences::ClearUser(kPrefRecentCrashes);
    return NS_ERROR_NOT_AVAILABLE;
  }

  bool inSafeMode = false;
  nsCOMPtr<nsIXULRuntime> xr = do_GetService(XULRUNTIME_SERVICE_CONTRACTID);
  NS_ENSURE_TRUE(xr, NS_ERROR_FAILURE);

  xr->GetInSafeMode(&inSafeMode);

  PRTime replacedLockTime;
  rv = xr->GetReplacedLockTime(&replacedLockTime);

  if (NS_FAILED(rv) || !replacedLockTime) {
    if (!inSafeMode) Preferences::ClearUser(kPrefRecentCrashes);
    GetAutomaticSafeModeNecessary(aIsSafeModeNecessary);
    return NS_OK;
  }

  int32_t maxResumedCrashes = -1;
  rv = Preferences::GetInt(kPrefMaxResumedCrashes, &maxResumedCrashes);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  int32_t recentCrashes = 0;
  Preferences::GetInt(kPrefRecentCrashes, &recentCrashes);
  mIsSafeModeNecessary =
      (recentCrashes > maxResumedCrashes && maxResumedCrashes != -1);

  char* xreProfilePath = PR_GetEnv("XRE_PROFILE_PATH");
  if (xreProfilePath) {
    GetAutomaticSafeModeNecessary(aIsSafeModeNecessary);
    return NS_ERROR_NOT_AVAILABLE;
  }

  int32_t lastSuccessfulStartup;
  rv = Preferences::GetInt(kPrefLastSuccess, &lastSuccessfulStartup);
  NS_ENSURE_SUCCESS(rv, rv);

  int32_t lockSeconds = (int32_t)(replacedLockTime / PR_MSEC_PER_SEC);

  if (lockSeconds <= lastSuccessfulStartup + MAX_STARTUP_BUFFER &&
      lockSeconds >= lastSuccessfulStartup - MAX_STARTUP_BUFFER) {
    GetAutomaticSafeModeNecessary(aIsSafeModeNecessary);
    return NS_OK;
  }

  if (PR_Now() / PR_USEC_PER_SEC <= lastSuccessfulStartup)
    return NS_ERROR_FAILURE;

  if (inSafeMode) {
    GetAutomaticSafeModeNecessary(aIsSafeModeNecessary);
    return NS_OK;
  }

  PRTime now = (PR_Now() / PR_USEC_PER_MSEC);
  if (replacedLockTime >= now - MAX_TIME_SINCE_STARTUP) {
    NS_WARNING("Last startup was detected as a crash.");
    recentCrashes++;
    rv = Preferences::SetInt(kPrefRecentCrashes, recentCrashes);
  } else {
    rv = Preferences::ClearUser(kPrefRecentCrashes);
  }
  NS_ENSURE_SUCCESS(rv, rv);

  mIsSafeModeNecessary =
      (recentCrashes > maxResumedCrashes && maxResumedCrashes != -1);

  nsCOMPtr<nsIPrefService> prefs = Preferences::GetService();
  rv = static_cast<Preferences*>(prefs.get())
           ->SavePrefFileBlocking();  
  NS_ENSURE_SUCCESS(rv, rv);

  GetAutomaticSafeModeNecessary(aIsSafeModeNecessary);
  return rv;
}

static nsresult RemoveIncompleteStartupFile() {
  nsCOMPtr<nsIFile> file;
  MOZ_TRY(NS_GetSpecialDirectory(NS_APP_USER_PROFILE_LOCAL_50_DIR,
                                 getter_AddRefs(file)));

  return NS_DispatchBackgroundTask(NS_NewRunnableFunction(
      "RemoveIncompleteStartupFile", [file = std::move(file)] {
        auto incompleteStartup =
            mozilla::startup::GetIncompleteStartupFile(file);
        if (NS_WARN_IF(incompleteStartup.isErr())) {
          return;
        }
        (void)NS_WARN_IF(NS_FAILED(incompleteStartup.unwrap()->Remove(false)));
      }));
}

NS_IMETHODIMP
nsAppStartup::TrackStartupCrashEnd() {
  bool inSafeMode = false;
  nsCOMPtr<nsIXULRuntime> xr = do_GetService(XULRUNTIME_SERVICE_CONTRACTID);
  if (xr) xr->GetInSafeMode(&inSafeMode);

  if (mStartupCrashAndHangTrackingEnded ||
      (mIsSafeModeNecessary && !inSafeMode))
    return NS_OK;
  mStartupCrashAndHangTrackingEnded = true;

  StartupTimeline::Record(StartupTimeline::STARTUP_CRASH_DETECTION_END);

  (void)NS_WARN_IF(NS_FAILED(RemoveIncompleteStartupFile()));

  TimeStamp mainTime = StartupTimeline::Get(StartupTimeline::MAIN);
  nsresult rv;

  if (mainTime.IsNull()) {
    NS_WARNING("Could not get StartupTimeline::MAIN time.");
  } else {
    uint64_t lockFileTime = ComputeAbsoluteTimestamp(mainTime);

    rv = Preferences::SetInt(kPrefLastSuccess,
                             (int32_t)(lockFileTime / PR_USEC_PER_SEC));

    if (NS_FAILED(rv))
      NS_WARNING("Could not set startup crash detection pref.");
  }

  if (inSafeMode && mIsSafeModeNecessary) {
    int32_t maxResumedCrashes = 0;
    nsIPrefBranch::PreferenceType prefType;
    rv = Preferences::GetRootBranch(PrefValueKind::Default)
             ->GetPrefType(kPrefMaxResumedCrashes, &prefType);
    NS_ENSURE_SUCCESS(rv, rv);
    if (prefType == nsIPrefBranch::PREF_INT) {
      rv = Preferences::GetInt(kPrefMaxResumedCrashes, &maxResumedCrashes);
      NS_ENSURE_SUCCESS(rv, rv);
    }
    rv = Preferences::SetInt(kPrefRecentCrashes, maxResumedCrashes);
    NS_ENSURE_SUCCESS(rv, rv);
  } else if (!inSafeMode) {
    rv = Preferences::ClearUser(kPrefRecentCrashes);
    if (NS_FAILED(rv)) NS_WARNING("Could not clear startup crash count.");
  }
  nsCOMPtr<nsIPrefService> prefs = Preferences::GetService();
  rv = prefs->SavePrefFile(nullptr);

  return rv;
}

NS_IMETHODIMP
nsAppStartup::RestartInSafeMode(uint32_t aQuitMode) {
  PR_SetEnv("MOZ_SAFE_MODE_RESTART=1");
  bool userAllowedQuit = false;
  this->Quit(aQuitMode | nsIAppStartup::eRestart, 0, &userAllowedQuit);

  return NS_OK;
}

NS_IMETHODIMP
nsAppStartup::CreateInstanceWithProfile(nsIToolkitProfile* aProfile,
                                        const nsTArray<nsString>& aArgs) {
  if (NS_WARN_IF(!aProfile)) {
    return NS_ERROR_FAILURE;
  }

  if (NS_WARN_IF(gAbsoluteArgv0Path.IsEmpty())) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIFile> execPath;
  nsresult rv = NS_NewLocalFile(gAbsoluteArgv0Path, getter_AddRefs(execPath));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCOMPtr<nsIProcess> process = do_CreateInstance(NS_PROCESS_CONTRACTID, &rv);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = process->Init(execPath);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsAutoCString profileName;
  rv = aProfile->GetName(profileName);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  NS_ConvertUTF8toUTF16 wideName(profileName);

  AutoTArray<const char16_t*, 2> args = {u"-P", wideName.get()};

  for (const auto& arg : aArgs) {
    args.AppendElement(arg.get());
  }

  rv = process->Runw(false, args.Elements(), args.Length());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}
