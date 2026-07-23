/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/ipc/GeckoChildProcessHost.h"

#include "mozilla/AppShutdown.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Components.h"
#include "mozilla/FilePreferences.h"
#include "mozilla/ChaosMode.h"
#include "mozilla/HelperMacros.h"
#include "mozilla/CmdLineAndEnvUtils.h"
#include "mozilla/IOInterposer.h"
#include "mozilla/ipc/UtilityProcessChild.h"
#include "mozilla/Likely.h"
#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "mozilla/PreferenceSheet.h"
#include "mozilla/Printf.h"
#include "mozilla/ProcessType.h"
#include "mozilla/ResultExtensions.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/StaticPrefs_browser.h"
#include "mozilla/StaticPrefs_fission.h"
#include "mozilla/StaticPrefs_security.h"
#include "mozilla/StaticPrefs_widget.h"
#include "mozilla/Try.h"
#include "mozilla/intl/LocaleService.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/widget/TextRecognition.h"
#include "mozJSModuleLoader.h"

#include "nsAppRunner.h"
#include "mozilla/XREAppData.h"
#include "mozilla/Bootstrap.h"
#if defined(MOZ_UPDATER) && !0
#  include "nsUpdateDriver.h"
#  include "nsUpdateSyncManager.h"
#endif

#include "prnetdb.h"
#include "prprf.h"
#include "prproces.h"
#include "prenv.h"
#include "prtime.h"

#include "nsIAppStartup.h"
#include "nsCategoryManagerUtils.h"
#include "nsIMutableArray.h"
#include "nsCommandLine.h"
#include "nsIComponentRegistrar.h"
#include "nsIDialogParamBlock.h"
#include "mozilla/ModuleUtils.h"
#include "nsIIOService.h"
#include "nsIObserverService.h"
#include "nsINativeAppSupport.h"
#include "nsIPlatformInfo.h"
#include "nsIProcess.h"
#include "nsIProfileUnlocker.h"
#include "nsIPromptService.h"
#include "nsIPropertyBag2.h"
#include "nsIServiceManager.h"
#include "nsIStringBundle.h"
#include "nsISupportsPrimitives.h"
#include "nsIToolkitProfile.h"
#include "nsToolkitProfileService.h"
#include "nsIURI.h"
#include "nsIURL.h"
#include "nsIWindowCreator.h"
#include "nsIWindowWatcher.h"
#include "nsIXULAppInfo.h"
#include "nsIXULRuntime.h"
#include "nsPIDOMWindow.h"
#include "nsIWidget.h"
#include "nsAppShellCID.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "mozilla/scache/StartupCache.h"
#include "gfxPlatform.h"
#include "PDMFactory.h"



#if defined(ACCESSIBILITY)
#  include "nsAccessibilityService.h"
#  include "mozilla/a11y/Platform.h"
#endif

#include "nsCRT.h"
#include "nsCOMPtr.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsEmbedCID.h"
#include "nsIDUtils.h"
#include "nsNetUtil.h"
#include "nsReadableUtils.h"
#include "nsXPCOM.h"
#include "nsXPCOMCIDInternal.h"
#include "nsString.h"
#include "nsPrintfCString.h"
#include "nsVersionComparator.h"

#include "nsAppDirectoryServiceDefs.h"
#include "nsXULAppAPI.h"
#include "nsXREDirProvider.h"

#include "nsINIParser.h"
#include "mozilla/Omnijar.h"
#include "mozilla/StartupTimeline.h"

#include <stdlib.h>

#if defined(XP_UNIX)
#  include <errno.h>
#  include <pwd.h>
#  include <string.h>
#  include <sys/resource.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif



#if defined(MOZ_HAS_REMOTE)
#  include "nsRemoteService.h"
#endif



#if defined(DEBUG)
#  include "mozilla/Logging.h"
#  include "mozilla/StaticPrefs_toolkit.h"
#endif

#include "nsIPrefService.h"
#include "js/String.h"
#  include "mozilla/widget/LSBUtils.h"

#include "base/command_line.h"



#if defined(MOZ_CODE_COVERAGE)
#  include "mozilla/CodeCoverageHandler.h"
#endif

#if defined(USE_GLX_TEST)
#  include "mozilla/GUniquePtr.h"
#  include "mozilla/GfxInfo.h"
#endif

#include "SafeMode.h"

#if defined(MOZ_BACKGROUNDTASKS)
#  include "mozilla/BackgroundTasks.h"
#  include "nsIPowerManagerService.h"
#  include "nsIStringBundle.h"
#endif

#if defined(MOZ_WIDGET_GTK)
#  include "nsAppShell.h"
#endif
#if defined(MOZ_ENABLE_DBUS)
#  include "DBusService.h"
#endif

extern void InstallSignalHandlers(const char* ProgramName);

#define FILE_COMPATIBILITY_INFO "compatibility.ini"_ns
#define FILE_INVALIDATE_CACHES ".purgecaches"_ns
#define FILE_STARTUP_INCOMPLETE u".startup-incomplete"_ns

#if defined(MOZ_DEFAULT_BROWSER_AGENT)
static const char kPrefDefaultAgentEnabled[] = "default-browser-agent.enabled";

static const char kPrefServicesSettingsServer[] = "services.settings.server";
static const char kPrefSetDefaultBrowserUserChoicePref[] =
    "browser.shell.setDefaultBrowserUserChoice";
#endif


#if defined(MOZ_WIDGET_GTK)
constexpr nsLiteralCString kStartupTokenNames[] = {
    "XDG_ACTIVATION_TOKEN"_ns,
    "DESKTOP_STARTUP_ID"_ns,
};
#endif

int gArgc;
char** gArgv;

static const char gToolkitVersion[] = MOZ_STRINGIFY(GRE_MILESTONE);
extern const char gToolkitBuildID[];

static nsIProfileLock* gProfileLock;
#if defined(MOZ_HAS_REMOTE)
constinit static RefPtr<nsRemoteService> gRemoteService;
constinit static RefPtr<nsStartupLock> gStartupLock;
#endif

int gRestartArgc;
char** gRestartArgv;

bool gRestartedByOS = false;

bool gIsGtest = false;

bool gKioskMode = false;
int gKioskMonitor = -1;

constinit nsString gAbsoluteArgv0Path;


#if defined(MOZ_WIDGET_GTK)
#  include <glib.h>
#  include "mozilla/WidgetUtilsGtk.h"
#  include <gtk/gtk.h>
#if defined(MOZ_WAYLAND)
#    include <gdk/gdkwayland.h>
#    include "mozilla/widget/nsWaylandDisplay.h"
#    include "wayland-proxy.h"
#endif
#endif

#if defined(MOZ_WAYLAND)
constinit std::unique_ptr<WaylandProxy> gWaylandProxy;
#endif

#include "BinaryPath.h"


#undef None

namespace mozilla {
bool RunningGTest() { return false; }
}  

using namespace mozilla;
using namespace mozilla::widget;
using namespace mozilla::startup;
using mozilla::dom::ContentChild;
using mozilla::dom::ContentParent;
using mozilla::dom::quota::QuotaManager;
using mozilla::intl::LocaleService;
using mozilla::scache::StartupCache;

static void MOZ_NEVER_INLINE SaveWordToEnv(const char* name,
                                           const nsACString& word) {
  char* expr =
      Smprintf("%s=%s", name, PromiseFlatCString(word).get()).release();
  if (expr) PR_SetEnv(expr);
}

static void SaveFileToEnv(const char* name, nsIFile* file) {
  nsAutoCString path;
  file->GetNativePath(path);
  SaveWordToEnv(name, path);
}

static bool gIsExpectedExit = false;

void MozExpectedExit() { gIsExpectedExit = true; }

static void UnexpectedExit() {
  if (!gIsExpectedExit) {
    gIsExpectedExit = true;  
    MOZ_CRASH("Exit called by third party code.");
  }
}

#if defined(MOZ_WAYLAND)
bool IsWaylandEnabled() {
  static bool isWaylandEnabled = []() {
    const char* waylandDisplay = PR_GetEnv("WAYLAND_DISPLAY");
    if (!waylandDisplay) {
      return false;
    }
    if (!PR_GetEnv("DISPLAY")) {
      return true;
    }
    if (const char* waylandPref = PR_GetEnv("MOZ_ENABLE_WAYLAND")) {
      return *waylandPref == '1';
    }
    if (const char* backendPref = PR_GetEnv("GDK_BACKEND")) {
      if (!strncmp(backendPref, "wayland", 7)) {
        NS_WARNING(
            "Wayland backend should be enabled by MOZ_ENABLE_WAYLAND=1."
            "GDK_BACKEND is a Gtk3 debug variable and may cause issues.");
        return true;
      }
    }
    return !gtk_check_version(3, 24, 30);
  }();
  return isWaylandEnabled;
}
#else
bool IsWaylandEnabled() { return false; }
#endif

static MOZ_FORMAT_PRINTF(2, 3) void Output(bool isError, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  vfprintf(stderr, fmt, ap);

  va_end(ap);
}

static ArgResult CheckArg(const char* aArg, const char** aParam = nullptr,
                          CheckArgFlag aFlags = CheckArgFlag::RemoveArg) {
  MOZ_ASSERT(gArgv, "gArgv must be initialized before CheckArg()");
  return CheckArg(gArgc, gArgv, aArg, aParam, aFlags);
}

static ArgResult CheckArgExists(const char* aArg) {
  return CheckArg(aArg, nullptr, CheckArgFlag::None);
}

bool gSafeMode = false;
bool gFxREmbedded = false;

enum E10sStatus {
  kE10sEnabledByDefault,
  kE10sForceDisabled,
};

static bool gBrowserTabsRemoteAutostart = false;
static E10sStatus gBrowserTabsRemoteStatus;
static bool gBrowserTabsRemoteAutostartInitialized = false;

namespace mozilla {

bool BrowserTabsRemoteAutostart() {
  if (gBrowserTabsRemoteAutostartInitialized) {
    return gBrowserTabsRemoteAutostart;
  }
  gBrowserTabsRemoteAutostartInitialized = true;

  if (!XRE_IsParentProcess()) {
    gBrowserTabsRemoteAutostart = true;
    return gBrowserTabsRemoteAutostart;
  }

  gBrowserTabsRemoteAutostart = true;
  E10sStatus status = kE10sEnabledByDefault;

#if defined(MOZILLA_OFFICIAL)
  bool allowDisablingE10s = false;
#else
  bool allowDisablingE10s = true;
#endif

  if (gBrowserTabsRemoteAutostart && allowDisablingE10s) {
    const char* forceDisable = PR_GetEnv("MOZ_FORCE_DISABLE_E10S");
    if (forceDisable && !strcmp(forceDisable, "1")) {
      gBrowserTabsRemoteAutostart = false;
      status = kE10sForceDisabled;
    }
  }

  gBrowserTabsRemoteStatus = status;

  return gBrowserTabsRemoteAutostart;
}

}  



static bool gWin32kInitialized = false;


static nsIXULRuntime::ContentWin32kLockdownState gWin32kStatus;


static nsIXULRuntime::ExperimentStatus gWin32kExperimentStatus =
    nsIXULRuntime::eExperimentStatusUnenrolled;


namespace mozilla {
void EnsureWin32kInitialized();
}

nsIXULRuntime::ContentWin32kLockdownState GetLiveWin32kLockdownState() {

  return nsIXULRuntime::ContentWin32kLockdownState::OperatingSystemNotSupported;

}

namespace mozilla {

void EnsureWin32kInitialized() {
  if (gWin32kInitialized) {
    return;
  }
  gWin32kInitialized = true;

  gWin32kStatus =
      nsIXULRuntime::ContentWin32kLockdownState::OperatingSystemNotSupported;
  gWin32kExperimentStatus = nsIXULRuntime::eExperimentStatusUnenrolled;

}

nsIXULRuntime::ContentWin32kLockdownState GetWin32kLockdownState() {

  return nsIXULRuntime::ContentWin32kLockdownState::OperatingSystemNotSupported;

}

}  



static const char kPrefFissionAutostart[] = "fission.autostart";

static const char kPrefFissionAutostartSession[] = "fission.autostart.session";


static bool gFissionAutostart = false;
static bool gFissionAutostartInitialized = false;
static nsIXULRuntime::FissionDecisionStatus gFissionDecisionStatus;
static void EnsureFissionAutostartInitialized() {
  if (gFissionAutostartInitialized) {
    return;
  }
  gFissionAutostartInitialized = true;

  if (!XRE_IsParentProcess()) {
    gFissionAutostart = Preferences::GetBool(kPrefFissionAutostartSession,
                                             false, PrefValueKind::Default);
    return;
  }

  if (!BrowserTabsRemoteAutostart()) {
    gFissionAutostart = false;
    if (gBrowserTabsRemoteStatus == kE10sForceDisabled) {
      gFissionDecisionStatus = nsIXULRuntime::eFissionDisabledByE10sEnv;
    } else {
      gFissionDecisionStatus = nsIXULRuntime::eFissionDisabledByE10sOther;
    }
  } else if (EnvHasValue("MOZ_FORCE_ENABLE_FISSION")) {
    gFissionAutostart = true;
    gFissionDecisionStatus = nsIXULRuntime::eFissionEnabledByEnv;
  } else if (EnvHasValue("MOZ_FORCE_DISABLE_FISSION")) {
    gFissionAutostart = false;
    gFissionDecisionStatus = nsIXULRuntime::eFissionDisabledByEnv;
  } else {
    gFissionAutostart = Preferences::GetBool(kPrefFissionAutostart, false);
    if (Preferences::HasUserValue(kPrefFissionAutostart)) {
      gFissionDecisionStatus = gFissionAutostart
                                   ? nsIXULRuntime::eFissionEnabledByUserPref
                                   : nsIXULRuntime::eFissionDisabledByUserPref;
    } else {
      gFissionDecisionStatus = gFissionAutostart
                                   ? nsIXULRuntime::eFissionEnabledByDefault
                                   : nsIXULRuntime::eFissionDisabledByDefault;
    }
  }

  Preferences::Unlock(kPrefFissionAutostartSession);
  Preferences::ClearUser(kPrefFissionAutostartSession);
  Preferences::SetBool(kPrefFissionAutostartSession, gFissionAutostart,
                       PrefValueKind::Default);
  Preferences::Lock(kPrefFissionAutostartSession);
}

namespace mozilla {

bool FissionAutostart() {
  EnsureFissionAutostartInitialized();
  return gFissionAutostart;
}

}  


namespace mozilla {

bool SessionStorePlatformCollection() {
  return !StaticPrefs::
      browser_sessionstore_disable_platform_collection_AtStartup_DoNotUseDirectly();
}

bool BFCacheInParent() {
  return StaticPrefs::fission_bfcacheInParent_DoNotUseDirectly();
}

}  

class nsXULAppInfo : public nsIXULAppInfo,
                     public nsIXULRuntime

{
 public:
  constexpr nsXULAppInfo() = default;
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIPLATFORMINFO
  NS_DECL_NSIXULAPPINFO
  NS_DECL_NSIXULRUNTIME
};

NS_INTERFACE_MAP_BEGIN(nsXULAppInfo)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIXULRuntime)
  NS_INTERFACE_MAP_ENTRY(nsIXULRuntime)
  NS_INTERFACE_MAP_ENTRY(nsIPlatformInfo)
  NS_INTERFACE_MAP_ENTRY_CONDITIONAL(nsIXULAppInfo,
                                     gAppData || XRE_IsContentProcess())
NS_INTERFACE_MAP_END

NS_IMETHODIMP_(MozExternalRefCountType)
nsXULAppInfo::AddRef() { return 1; }

NS_IMETHODIMP_(MozExternalRefCountType)
nsXULAppInfo::Release() { return 1; }

NS_IMETHODIMP
nsXULAppInfo::GetVendor(nsACString& aResult) {
  if (XRE_IsContentProcess()) {
    ContentChild* cc = ContentChild::GetSingleton();
    aResult = cc->GetAppInfo().vendor;
    return NS_OK;
  }
  aResult.Assign(gAppData->vendor);

  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetName(nsACString& aResult) {
  if (XRE_IsContentProcess()) {
    ContentChild* cc = ContentChild::GetSingleton();
    aResult = cc->GetAppInfo().name;
    return NS_OK;
  }

  aResult.Assign(gAppData->name);

  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetID(nsACString& aResult) {
  if (XRE_IsContentProcess()) {
    ContentChild* cc = ContentChild::GetSingleton();
    aResult = cc->GetAppInfo().ID;
    return NS_OK;
  }
  aResult.Assign(gAppData->ID);

  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetVersion(nsACString& aResult) {
  if (XRE_IsContentProcess()) {
    ContentChild* cc = ContentChild::GetSingleton();
    aResult = cc->GetAppInfo().version;
    return NS_OK;
  }
  aResult.Assign(gAppData->version);

  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetPlatformVersion(nsACString& aResult) {
  aResult.Assign(gToolkitVersion);

  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetAppBuildID(nsACString& aResult) {
  if (XRE_IsContentProcess()) {
    ContentChild* cc = ContentChild::GetSingleton();
    aResult = cc->GetAppInfo().buildID;
    return NS_OK;
  }
  aResult.Assign(gAppData->buildID);

  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetPlatformBuildID(nsACString& aResult) {
  aResult.Assign(gToolkitBuildID);

  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetUAName(nsACString& aResult) {
  if (XRE_IsContentProcess()) {
    ContentChild* cc = ContentChild::GetSingleton();
    aResult = cc->GetAppInfo().UAName;
    return NS_OK;
  }
  aResult.Assign(gAppData->UAName);

  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetSourceURL(nsACString& aResult) {
  if (XRE_IsContentProcess()) {
    ContentChild* cc = ContentChild::GetSingleton();
    aResult = cc->GetAppInfo().sourceURL;
    return NS_OK;
  }
  aResult.Assign(gAppData->sourceURL);

  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetUpdateURL(nsACString& aResult) {
  if (XRE_IsContentProcess()) {
    ContentChild* cc = ContentChild::GetSingleton();
    aResult = cc->GetAppInfo().updateURL;
    return NS_OK;
  }
  aResult.Assign(gAppData->updateURL);

  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetRemotingName(nsACString& aResult) {
  if (XRE_IsContentProcess()) {
    MOZ_ASSERT(false,
               "nsXULAppInfo::remotingName should not be accessed from the "
               "content process");
    return NS_ERROR_UNEXPECTED;
  }
  aResult.Assign(gAppData->remotingName);

  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetLogConsoleErrors(bool* aResult) {
  *aResult = gLogConsoleErrors;
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::SetLogConsoleErrors(bool aValue) {
  gLogConsoleErrors = aValue;
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetInSafeMode(bool* aResult) {
  *aResult = gSafeMode;
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetOS(nsACString& aResult) {
  aResult.AssignLiteral(OS_TARGET);
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetXPCOMABI(nsACString& aResult) {
#if defined(TARGET_XPCOM_ABI)
  aResult.AssignLiteral(TARGET_XPCOM_ABI);
  return NS_OK;
#else
  return NS_ERROR_NOT_AVAILABLE;
#endif
}

NS_IMETHODIMP
nsXULAppInfo::GetWidgetToolkit(nsACString& aResult) {
  aResult.AssignLiteral(MOZ_WIDGET_TOOLKIT);
  return NS_OK;
}

#define GECKO_PROCESS_TYPE(enum_value, enum_name, string_name, proc_typename, \
                           process_bin_type, procinfo_typename,               \
                           webidl_typename, allcaps_name)                     \
  static_assert(nsIXULRuntime::PROCESS_TYPE_##allcaps_name ==                 \
                    static_cast<int>(GeckoProcessType_##enum_name),           \
                "GeckoProcessType in nsXULAppAPI.h not synchronized with "    \
                "nsIXULRuntime.idl");
#include "mozilla/GeckoProcessTypes.h"
#undef GECKO_PROCESS_TYPE

static_assert(GeckoProcessType_Utility + 1 == GeckoProcessType_End,
              "Did not find the final GeckoProcessType");

NS_IMETHODIMP
nsXULAppInfo::GetProcessType(uint32_t* aResult) {
  NS_ENSURE_ARG_POINTER(aResult);
  *aResult = XRE_GetProcessType();
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetProcessID(uint32_t* aResult) {
  *aResult = getpid();
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetUniqueProcessID(uint64_t* aResult) {
  if (XRE_IsContentProcess()) {
    ContentChild* cc = ContentChild::GetSingleton();
    *aResult = cc->GetID();
  } else {
    *aResult = 0;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetRemoteType(nsACString& aRemoteType) {
  if (XRE_IsContentProcess()) {
    aRemoteType = ContentChild::GetSingleton()->GetRemoteType();
  } else {
    aRemoteType = NOT_REMOTE_TYPE;
  }

  return NS_OK;
}

constinit static nsCString gLastAppVersion;
constinit static nsCString gLastAppBuildID;

static bool gProfileEncryptedDatabases = false;

NS_IMETHODIMP
nsXULAppInfo::GetLastAppVersion(nsACString& aResult) {
  if (XRE_IsContentProcess()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  if (!gLastAppVersion.IsVoid() && gLastAppVersion.IsEmpty()) {
    NS_WARNING("Attempt to retrieve lastAppVersion before it has been set.");
    return NS_ERROR_NOT_AVAILABLE;
  }

  aResult.Assign(gLastAppVersion);
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetLastAppBuildID(nsACString& aResult) {
  if (XRE_IsContentProcess()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  if (!gLastAppBuildID.IsVoid() && gLastAppBuildID.IsEmpty()) {
    NS_WARNING("Attempt to retrieve lastAppBuildID before it has been set.");
    return NS_ERROR_NOT_AVAILABLE;
  }

  aResult.Assign(gLastAppBuildID);
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetFissionAutostart(bool* aResult) {
  *aResult = FissionAutostart();
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetWin32kExperimentStatus(ExperimentStatus* aResult) {
  if (!XRE_IsParentProcess()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  EnsureWin32kInitialized();
  *aResult = gWin32kExperimentStatus;
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetWin32kLiveStatusTestingOnly(
    nsIXULRuntime::ContentWin32kLockdownState* aResult) {
  if (!XRE_IsParentProcess()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  EnsureWin32kInitialized();
  *aResult = GetLiveWin32kLockdownState();
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetWin32kSessionStatus(
    nsIXULRuntime::ContentWin32kLockdownState* aResult) {
  if (!XRE_IsParentProcess()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  EnsureWin32kInitialized();
  *aResult = gWin32kStatus;
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetFissionDecisionStatus(FissionDecisionStatus* aResult) {
  if (!XRE_IsParentProcess()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  EnsureFissionAutostartInitialized();

  MOZ_ASSERT(gFissionDecisionStatus != eFissionStatusUnknown);
  *aResult = gFissionDecisionStatus;
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetFissionDecisionStatusString(nsACString& aResult) {
  if (!XRE_IsParentProcess()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  EnsureFissionAutostartInitialized();
  switch (gFissionDecisionStatus) {
    case eFissionDisabledByE10sEnv:
      aResult = "disabledByE10sEnv";
      break;
    case eFissionEnabledByEnv:
      aResult = "enabledByEnv";
      break;
    case eFissionDisabledByEnv:
      aResult = "disabledByEnv";
      break;
    case eFissionEnabledByDefault:
      aResult = "enabledByDefault";
      break;
    case eFissionDisabledByDefault:
      aResult = "disabledByDefault";
      break;
    case eFissionEnabledByUserPref:
      aResult = "enabledByUserPref";
      break;
    case eFissionDisabledByUserPref:
      aResult = "disabledByUserPref";
      break;
    case eFissionDisabledByE10sOther:
      aResult = "disabledByE10sOther";
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("Unexpected enum value");
  }
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetSessionStorePlatformCollection(bool* aResult) {
  *aResult = SessionStorePlatformCollection();
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetBrowserTabsRemoteAutostart(bool* aResult) {
  *aResult = BrowserTabsRemoteAutostart();
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetMaxWebProcessCount(uint32_t* aResult) {
  *aResult = mozilla::GetMaxWebProcessCount();
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetAccessibilityEnabled(bool* aResult) {
#if defined(ACCESSIBILITY)
  *aResult = GetAccService() != nullptr;
#else
  *aResult = false;
#endif
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetAccessibilityInstantiator(nsAString& aInstantiator) {
  aInstantiator.Truncate();
#if defined(ACCESSIBILITY)
  if (GetAccService()) {
    a11y::GetHumanReadableInstantiatorStr(aInstantiator);
  }
#endif
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetIs64Bit(bool* aResult) {
#if defined(HAVE_64BIT_BUILD)
  *aResult = true;
#else
  *aResult = false;
#endif
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetIsTextRecognitionSupported(bool* aResult) {
  *aResult = widget::TextRecognition::IsSupported();
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::InvalidateCachesOnRestart() {
  nsCOMPtr<nsIFile> file;
  nsresult rv =
      NS_GetSpecialDirectory(NS_APP_PROFILE_DIR_STARTUP, getter_AddRefs(file));
  if (NS_FAILED(rv)) return rv;
  if (!file) return NS_ERROR_NOT_AVAILABLE;

  file->AppendNative(FILE_COMPATIBILITY_INFO);

  nsINIParser parser;
  rv = parser.Init(file);
  if (NS_FAILED(rv)) {
    return NS_OK;
  }

  nsAutoCString buf;
  rv = parser.GetString("Compatibility", "InvalidateCaches", buf);

  if (NS_FAILED(rv)) {
    PRFileDesc* fd;
    rv = file->OpenNSPRFileDesc(PR_RDWR | PR_APPEND, 0600, &fd);
    if (NS_FAILED(rv)) {
      NS_ERROR("could not create output stream");
      return NS_ERROR_NOT_AVAILABLE;
    }
    static const char kInvalidationHeader[] =
        NS_LINEBREAK "InvalidateCaches=1" NS_LINEBREAK;
    PR_Write(fd, kInvalidationHeader, sizeof(kInvalidationHeader) - 1);
    PR_Close(fd);
  }
  return NS_OK;
}

nsresult mozilla::MarkProfileEncryptedDatabases() {
  nsCOMPtr<nsIFile> file;
  nsresult rv =
      NS_GetSpecialDirectory(NS_APP_PROFILE_DIR_STARTUP, getter_AddRefs(file));
  if (NS_FAILED(rv) || !file) return NS_OK;

  file->AppendNative(FILE_COMPATIBILITY_INFO);

  nsINIParser parser;
  rv = parser.Init(file);
  if (NS_FAILED(rv)) {
    return NS_OK;
  }

  nsAutoCString buf;
  rv = parser.GetString("Compatibility", "EncryptedDatabases", buf);
  if (NS_SUCCEEDED(rv)) {
    return NS_OK;
  }

  PRFileDesc* fd;
  rv = file->OpenNSPRFileDesc(PR_RDWR | PR_APPEND, 0600, &fd);
  if (NS_FAILED(rv)) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  static const char kEncryptedHeader[] =
      NS_LINEBREAK "EncryptedDatabases=1" NS_LINEBREAK;
  PR_Write(fd, kEncryptedHeader, sizeof(kEncryptedHeader) - 1);
  PR_Close(fd);
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::MarkProfileEncryptedDatabases() {
  return mozilla::MarkProfileEncryptedDatabases();
}

NS_IMETHODIMP
nsXULAppInfo::GetReplacedLockTime(PRTime* aReplacedLockTime) {
  if (!gProfileLock) return NS_ERROR_NOT_AVAILABLE;
  gProfileLock->GetReplacedLockTime(aReplacedLockTime);
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetDefaultUpdateChannel(nsACString& aResult) {
  aResult.AssignLiteral(MOZ_STRINGIFY(MOZ_UPDATE_CHANNEL));
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetDistributionID(nsACString& aResult) {
  aResult.AssignLiteral(MOZ_DISTRIBUTION_ID);
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetWindowsDLLBlocklistStatus(bool* aResult) {
#if defined(HAS_DLL_BLOCKLIST)
  *aResult = DllBlocklist_CheckStatus();
#else
  *aResult = false;
#endif
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetRestartedByOS(bool* aResult) {
  *aResult = gRestartedByOS;
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetChromeColorSchemeIsDark(bool* aResult) {
  *aResult = PreferenceSheet::ColorSchemeForChrome() == ColorScheme::Dark;
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetNativeMenubar(bool* aResult) {
  *aResult = !!LookAndFeel::GetInt(LookAndFeel::IntID::NativeMenubar);
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetContentThemeDerivedColorSchemeIsDark(bool* aResult) {
  *aResult =
      PreferenceSheet::ThemeDerivedColorSchemeForContent() == ColorScheme::Dark;
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetPrefersReducedMotion(bool* aResult) {
  *aResult =
      LookAndFeel::GetInt(LookAndFeel::IntID::PrefersReducedMotion, 0) == 1;
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetDrawInTitlebar(bool* aResult) {
  *aResult = LookAndFeel::DrawInTitlebar();
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetCaretBlinkCount(int32_t* aResult) {
  *aResult = LookAndFeel::CaretBlinkCount();
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetCaretBlinkTime(int32_t* aResult) {
  *aResult = LookAndFeel::CaretBlinkTime();
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetDesktopEnvironment(nsACString& aDesktopEnvironment) {
#if defined(MOZ_WIDGET_GTK)
  aDesktopEnvironment.Assign(GetDesktopEnvironmentIdentifier());
#endif
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetIsWayland(bool* aResult) {
#if defined(MOZ_WIDGET_GTK)
  *aResult = GdkIsWaylandDisplay();
#else
  *aResult = false;
#endif
  return NS_OK;
}

NS_IMETHODIMP
nsXULAppInfo::GetProcessStartupShortcut(nsAString& aShortcut) {
  return NS_ERROR_NOT_AVAILABLE;
}


NS_IMETHODIMP
nsXULAppInfo::GetLauncherProcessState(uint32_t* aResult) {
  return NS_ERROR_NOT_AVAILABLE;
}


static const nsXULAppInfo kAppInfo;
namespace mozilla {
nsresult AppInfoConstructor(REFNSIID aIID, void** aResult) {
  return const_cast<nsXULAppInfo*>(&kAppInfo)->QueryInterface(aIID, aResult);
}
}  

bool gLogConsoleErrors = false;

#define NS_ENSURE_TRUE_LOG(x, ret)               \
  PR_BEGIN_MACRO                                 \
  if (MOZ_UNLIKELY(!(x))) {                      \
    NS_WARNING("NS_ENSURE_TRUE(" #x ") failed"); \
    gLogConsoleErrors = true;                    \
    return ret;                                  \
  }                                              \
  PR_END_MACRO

#define NS_ENSURE_SUCCESS_LOG(res, ret) \
  NS_ENSURE_TRUE_LOG(NS_SUCCEEDED(res), ret)


class ScopedXPCOMStartup {
 public:
  ScopedXPCOMStartup() : mServiceManager(nullptr) {}
  ~ScopedXPCOMStartup();

  nsresult Initialize(bool aInitJSContext = true);
  nsresult SetWindowCreator(nsINativeAppSupport* native);

 private:
  nsIServiceManager* mServiceManager;
  static nsINativeAppSupport* gNativeAppSupport;

  friend already_AddRefed<nsINativeAppSupport> NS_GetNativeAppSupport();
};

ScopedXPCOMStartup::~ScopedXPCOMStartup() {
  NS_IF_RELEASE(gNativeAppSupport);

  if (mServiceManager) {

    nsCOMPtr<nsIAppStartup> appStartup(components::AppStartup::Service());
    if (appStartup) appStartup->DestroyHiddenWindow();

    gDirServiceProvider->DoShutdown();

    WriteConsoleLog();

    NS_ShutdownXPCOM(mServiceManager);
    mServiceManager = nullptr;
  }
}

nsresult ScopedXPCOMStartup::Initialize(bool aInitJSContext) {
  NS_ASSERTION(gDirServiceProvider, "Should not get here!");

  nsresult rv;

  rv = NS_InitXPCOM(&mServiceManager, gDirServiceProvider->GetAppDir(),
                    gDirServiceProvider, aInitJSContext);
  if (NS_FAILED(rv)) {
    NS_ERROR("Couldn't start xpcom!");
    mServiceManager = nullptr;
  } else {
#if defined(DEBUG)
    nsCOMPtr<nsIComponentRegistrar> reg = do_QueryInterface(mServiceManager);
    NS_ASSERTION(reg, "Service Manager doesn't QI to Registrar.");
#endif
  }

  return rv;
}

class nsSingletonFactory final : public nsIFactory {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIFACTORY

  explicit nsSingletonFactory(nsISupports* aSingleton);

 private:
  ~nsSingletonFactory() = default;
  nsCOMPtr<nsISupports> mSingleton;
};

nsSingletonFactory::nsSingletonFactory(nsISupports* aSingleton)
    : mSingleton(aSingleton) {
  NS_ASSERTION(mSingleton, "Singleton was null!");
}

NS_IMPL_ISUPPORTS(nsSingletonFactory, nsIFactory)

NS_IMETHODIMP
nsSingletonFactory::CreateInstance(const nsIID& aIID, void** aResult) {
  return mSingleton->QueryInterface(aIID, aResult);
}

nsresult ScopedXPCOMStartup::SetWindowCreator(nsINativeAppSupport* native) {
  nsresult rv;

  NS_IF_ADDREF(gNativeAppSupport = native);

  nsCOMPtr<nsIWindowCreator> creator(components::AppStartup::Service());
  if (!creator) return NS_ERROR_UNEXPECTED;

  nsCOMPtr<nsIWindowWatcher> wwatch(
      do_GetService(NS_WINDOWWATCHER_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  return wwatch->SetWindowCreator(creator);
}

 already_AddRefed<nsINativeAppSupport> NS_GetNativeAppSupport() {
  if (!ScopedXPCOMStartup::gNativeAppSupport) {
    return nullptr;
  }

  return do_AddRef(ScopedXPCOMStartup::gNativeAppSupport);
}

#if defined(MOZ_HAS_REMOTE)
 already_AddRefed<nsIRemoteService> GetRemoteService() {
  AssertIsOnMainThread();

  if (!gRemoteService) {
    gRemoteService = new nsRemoteService();
  }
  nsCOMPtr<nsIRemoteService> remoteService = gRemoteService.get();
  return remoteService.forget();
}
#endif

nsINativeAppSupport* ScopedXPCOMStartup::gNativeAppSupport;

static void DumpArbitraryHelp() {
  nsresult rv;

  ScopedLogging log;

  {
    ScopedXPCOMStartup xpcom;
    xpcom.Initialize();

    nsCOMPtr<nsICommandLineRunner> cmdline(new nsCommandLine());

    nsCString text;
    rv = cmdline->GetHelpText(text);
    if (NS_SUCCEEDED(rv)) printf("%s", text.get());
  }
}

static void DumpHelp() {
  printf(
      "Usage: %s [ options ... ] [URL]\n"
      "       where options include:\n\n",
      gArgv[0]);

#if defined(XP_UNIX)
  printf(
      "  --g-fatal-warnings Make all warnings fatal\n"
      "\n%s options\n",
      (const char*)gAppData->name);
#endif

  printf(
      "  -h or --help       Print this message.\n"
      "  -v or --version    Print %s version.\n"
      "  --full-version     Print %s version, build and platform build ids.\n"
      "  -P <profile>       Start with <profile>.\n"
      "  --profile <path>   Start with profile at <path>.\n"
      "  --migration        Start with migration wizard.\n"
      "  --ProfileManager   Start with ProfileManager.\n"
      "  --origin-to-force-quic-on <origin>\n"
      "                     Force to use QUIC for the specified origin.\n"
#if defined(MOZ_HAS_REMOTE)
      "  --new-instance     Open new instance, not a new window in running "
      "instance.\n"
#endif
      "  --safe-mode        Disables extensions and themes for this session.\n"
#if defined(MOZ_BLOCK_PROFILE_DOWNGRADE)
      "  --allow-downgrade  Allows downgrading a profile.\n"
#endif
      "  --MOZ_LOG=<modules> Treated as MOZ_LOG=<modules> environment "
      "variable,\n"
      "                     overrides it.\n"
      "  --MOZ_LOG_FILE=<file> Treated as MOZ_LOG_FILE=<file> environment "
      "variable,\n"
      "                     overrides it. If MOZ_LOG_FILE is not specified as "
      "an\n"
      "                     argument or as an environment variable, logging "
      "will be\n"
      "                     written to stdout.\n",
      (const char*)gAppData->name, (const char*)gAppData->name);


#if 0 || defined(MOZ_WIDGET_GTK) || 0
  printf("  --headless         Run without a GUI.\n");
#endif

#if defined(MOZ_ENABLE_DBUS)
  printf(
      "  --dbus-service <launcher>  Run as DBus service for "
      "org.freedesktop.Application and\n"
      "                             set a launcher (usually /usr/bin/appname "
      "script) for it.\n");
#endif

  DumpArbitraryHelp();
}

static inline void DumpVersion() {
  if (gAppData->vendor && *gAppData->vendor) {
    printf("%s ", (const char*)gAppData->vendor);
  }
  printf("%s ", (const char*)gAppData->name);

  printf("%s", MOZ_STRINGIFY(MOZ_APP_VERSION_DISPLAY));

  if (gAppData->copyright && *gAppData->copyright) {
    printf(", %s", (const char*)gAppData->copyright);
  }
  printf("\n");
}

static inline void DumpFullVersion() {
  if (gAppData->vendor && *gAppData->vendor) {
    printf("%s ", (const char*)gAppData->vendor);
  }
  printf("%s ", (const char*)gAppData->name);

  printf("%s ", MOZ_STRINGIFY(MOZ_APP_VERSION_DISPLAY));

  printf("%s ", (const char*)gAppData->buildID);
  printf("%s ", (const char*)PlatformBuildID());
  if (gAppData->copyright && *gAppData->copyright) {
    printf(", %s", (const char*)gAppData->copyright);
  }
  printf("\n");
}

void XRE_InitOmnijar(nsIFile* greOmni, nsIFile* appOmni) {
  mozilla::Omnijar::Init(greOmni, appOmni);
}

nsresult XRE_GetBinaryPath(nsIFile** aResult) {
  return mozilla::BinaryPath::GetFile(aResult);
}


void UnlockProfile() {
  if (gProfileLock) {
    gProfileLock->Unlock();
  }
}

nsresult LaunchChild(bool aBlankCommandLine, bool aTryExec) {

  if (aBlankCommandLine) {
    gRestartArgc = 1;
    gRestartArgv[gRestartArgc] = nullptr;
  }

  SaveToEnv("MOZ_LAUNCHED_CHILD=1");

#if !0  // Android has separate restart code.
  nsCOMPtr<nsIFile> lf;
  nsresult rv = XRE_GetBinaryPath(getter_AddRefs(lf));
  if (NS_FAILED(rv)) return rv;

  nsAutoCString exePath;
  rv = lf->GetNativePath(exePath);
  if (NS_FAILED(rv)) return rv;

#if defined(XP_UNIX)
  if (aTryExec) {
    execv(exePath.get(), gRestartArgv);

    return NS_ERROR_FAILURE;
  }
#endif
  if (PR_CreateProcessDetached(exePath.get(), gRestartArgv, nullptr, nullptr) ==
      PR_FAILURE) {
    return NS_ERROR_FAILURE;
  }


#endif

  return NS_ERROR_LAUNCHED_CHILD_PROCESS;
}

static const char kProfileProperties[] =
    "chrome://mozapps/locale/profile/profileSelection.properties";

namespace {

class ReturnAbortOnError {
 public:
  MOZ_IMPLICIT ReturnAbortOnError(nsresult aRv) { mRv = ConvertRv(aRv); }

  operator nsresult() { return mRv; }

 private:
  inline nsresult ConvertRv(nsresult aRv) {
    if (NS_SUCCEEDED(aRv) || aRv == NS_ERROR_LAUNCHED_CHILD_PROCESS) {
      return aRv;
    }
#if defined(MOZ_BACKGROUNDTASKS)
    if (aRv == NS_ERROR_UNEXPECTED && BackgroundTasks::IsBackgroundTaskMode()) {
      return aRv;
    }
#endif
    return NS_ERROR_ABORT;
  }

  nsresult mRv;
};

}  

static nsresult ProfileMissingDialog(nsINativeAppSupport* aNative) {
#if defined(MOZ_BACKGROUNDTASKS)
  if (BackgroundTasks::IsBackgroundTaskMode()) {
    printf_stderr(
        "Could not determine any profile running in backgroundtask mode!\n");
    return NS_ERROR_ABORT;
  }
#endif

  nsresult rv;

  ScopedXPCOMStartup xpcom;
  rv = xpcom.Initialize();
  NS_ENSURE_SUCCESS(rv, rv);

  rv = xpcom.SetWindowCreator(aNative);
  NS_ENSURE_SUCCESS(rv, NS_ERROR_FAILURE);


  {  
    nsCOMPtr<nsIStringBundleService> sbs =
        mozilla::components::StringBundle::Service();
    NS_ENSURE_TRUE(sbs, NS_ERROR_FAILURE);

    nsCOMPtr<nsIStringBundle> sb;
    sbs->CreateBundle(kProfileProperties, getter_AddRefs(sb));
    NS_ENSURE_TRUE_LOG(sb, NS_ERROR_FAILURE);

    NS_ConvertUTF8toUTF16 appName(gAppData->name);
    AutoTArray<nsString, 2> params = {appName, appName};

    nsAutoString missingMessage;
    rv = sb->FormatStringFromName("profileMissing", params, missingMessage);
    NS_ENSURE_SUCCESS(rv, NS_ERROR_ABORT);

    nsAutoString missingTitle;
    params.SetLength(1);
    rv = sb->FormatStringFromName("profileMissingTitle", params, missingTitle);
    NS_ENSURE_SUCCESS(rv, NS_ERROR_ABORT);

    nsCOMPtr<nsIPromptService> ps(do_GetService(NS_PROMPTSERVICE_CONTRACTID));
    NS_ENSURE_TRUE(ps, NS_ERROR_FAILURE);

    ps->Alert(nullptr, missingTitle.get(), missingMessage.get());

    return NS_ERROR_ABORT;
  }
}

static nsresult ProfileEncryptionMismatchDialog(const char* aMsgKey,
                                                const char* aTitleKey,
                                                nsINativeAppSupport* aNative) {
#if defined(MOZ_BACKGROUNDTASKS)
  if (BackgroundTasks::IsBackgroundTaskMode()) {
    printf_stderr(
        "Profile encryption state does not match launching build in "
        "backgroundtask mode\n");
    return NS_ERROR_ABORT;
  }
#endif

  nsresult rv;

  ScopedXPCOMStartup xpcom;
  rv = xpcom.Initialize();
  NS_ENSURE_SUCCESS(rv, rv);

  rv = xpcom.SetWindowCreator(aNative);
  NS_ENSURE_SUCCESS(rv, NS_ERROR_FAILURE);


  {  
    nsCOMPtr<nsIStringBundleService> sbs =
        mozilla::components::StringBundle::Service();
    NS_ENSURE_TRUE(sbs, NS_ERROR_FAILURE);

    nsCOMPtr<nsIStringBundle> sb;
    sbs->CreateBundle(kProfileProperties, getter_AddRefs(sb));
    NS_ENSURE_TRUE_LOG(sb, NS_ERROR_FAILURE);

    NS_ConvertUTF8toUTF16 appName(gAppData->name);
    AutoTArray<nsString, 3> params = {appName, appName, appName};

    nsAutoString msg;
    rv = sb->FormatStringFromName(aMsgKey, params, msg);
    NS_ENSURE_SUCCESS(rv, NS_ERROR_ABORT);

    nsAutoString title;
    params.SetLength(1);
    rv = sb->FormatStringFromName(aTitleKey, params, title);
    NS_ENSURE_SUCCESS(rv, NS_ERROR_ABORT);

    nsCOMPtr<nsIPromptService> ps(do_GetService(NS_PROMPTSERVICE_CONTRACTID));
    NS_ENSURE_TRUE(ps, NS_ERROR_FAILURE);

    ps->Alert(nullptr, title.get(), msg.get());
    return NS_ERROR_ABORT;
  }
}

static ReturnAbortOnError ProfileLockedDialog(nsIFile* aProfileDir,
                                              nsIFile* aProfileLocalDir,
                                              nsIProfileUnlocker* aUnlocker,
                                              nsINativeAppSupport* aNative,
                                              nsIProfileLock** aResult) {
  nsresult rv;

#if defined(MOZ_HAS_REMOTE)
  gStartupLock = nullptr;
#endif

  bool exists;
  aProfileDir->Exists(&exists);
  if (!exists) {
    return ProfileMissingDialog(aNative);
  }

  ScopedXPCOMStartup xpcom;
  rv = xpcom.Initialize();
  NS_ENSURE_SUCCESS(rv, rv);

  rv = xpcom.SetWindowCreator(aNative);
  NS_ENSURE_SUCCESS(rv, NS_ERROR_FAILURE);


  {  
    nsCOMPtr<nsIStringBundleService> sbs =
        mozilla::components::StringBundle::Service();
    NS_ENSURE_TRUE(sbs, NS_ERROR_FAILURE);

    nsCOMPtr<nsIStringBundle> sb;
    sbs->CreateBundle(kProfileProperties, getter_AddRefs(sb));
    NS_ENSURE_TRUE_LOG(sbs, NS_ERROR_FAILURE);

    NS_ConvertUTF8toUTF16 appName(gAppData->name);
    AutoTArray<nsString, 3> params = {appName, appName, appName};

    nsAutoString killMessage;
    rv = sb->FormatStringFromName(
        aUnlocker ? "restartMessageUnlocker" : "restartMessageNoUnlocker2",
        params, killMessage);
    NS_ENSURE_SUCCESS(rv, NS_ERROR_FAILURE);

    params.SetLength(1);
    nsAutoString killTitle;
    rv = sb->FormatStringFromName("restartTitle", params, killTitle);
    NS_ENSURE_SUCCESS(rv, NS_ERROR_FAILURE);

#if defined(MOZ_BACKGROUNDTASKS)
    if (BackgroundTasks::IsBackgroundTaskMode()) {
      printf_stderr("%s\n", NS_LossyConvertUTF16toASCII(killMessage).get());
      return NS_ERROR_UNEXPECTED;
    }
#endif

    nsCOMPtr<nsIPromptService> ps(do_GetService(NS_PROMPTSERVICE_CONTRACTID));
    NS_ENSURE_TRUE(ps, NS_ERROR_FAILURE);

    if (aUnlocker) {
      int32_t button;
      const uint32_t flags = (nsIPromptService::BUTTON_TITLE_IS_STRING *
                              nsIPromptService::BUTTON_POS_0) +
                             (nsIPromptService::BUTTON_TITLE_CANCEL *
                              nsIPromptService::BUTTON_POS_1);

      bool checkState = false;
      rv = ps->ConfirmEx(nullptr, killTitle.get(), killMessage.get(), flags,
                         killTitle.get(), nullptr, nullptr, nullptr,
                         &checkState, &button);
      NS_ENSURE_SUCCESS_LOG(rv, rv);

      if (button == 0) {
        rv = aUnlocker->Unlock(nsIProfileUnlocker::FORCE_QUIT);
        if (NS_FAILED(rv)) {
          return rv;
        }

        SaveFileToEnv("XRE_PROFILE_PATH", aProfileDir);
        SaveFileToEnv("XRE_PROFILE_LOCAL_PATH", aProfileLocalDir);

        return LaunchChild(false, true);
      }
    } else {
      rv = ps->Alert(nullptr, killTitle.get(), killMessage.get());
      NS_ENSURE_SUCCESS_LOG(rv, rv);
    }

    return NS_ERROR_ABORT;
  }
}

static ReturnAbortOnError ShowProfileDialog(
    nsIToolkitProfileService* aProfileSvc, nsINativeAppSupport* aNative,
    const char* aDialogURL) {
  nsresult rv;

  nsCOMPtr<nsIFile> profD, profLD;
  bool offline = false;
  int32_t dialogReturn;

#if defined(MOZ_HAS_REMOTE)
  gStartupLock = nullptr;
#endif

  {
    ScopedXPCOMStartup xpcom;
    rv = xpcom.Initialize();
    NS_ENSURE_SUCCESS(rv, rv);

    rv = xpcom.SetWindowCreator(aNative);
    NS_ENSURE_SUCCESS(rv, NS_ERROR_FAILURE);


    {  
      nsCOMPtr<nsIWindowWatcher> windowWatcher(
          do_GetService(NS_WINDOWWATCHER_CONTRACTID));
      nsCOMPtr<nsIDialogParamBlock> ioParamBlock(
          do_CreateInstance(NS_DIALOGPARAMBLOCK_CONTRACTID));
      nsCOMPtr<nsIMutableArray> dlgArray(
          do_CreateInstance(NS_ARRAY_CONTRACTID));
      NS_ENSURE_TRUE(windowWatcher && ioParamBlock && dlgArray,
                     NS_ERROR_FAILURE);

      ioParamBlock->SetObjects(dlgArray);

      nsCOMPtr<nsIAppStartup> appStartup(components::AppStartup::Service());
      NS_ENSURE_TRUE(appStartup, NS_ERROR_FAILURE);

      nsAutoCString features("centerscreen,chrome,modal,titlebar");
      if (CheckArgExists("private-window") == ARG_FOUND) {
        features.AppendLiteral(",private");
      }
      nsCOMPtr<mozIDOMWindowProxy> newWindow;
      rv = windowWatcher->OpenWindow(nullptr, nsDependentCString(aDialogURL),
                                     "_blank"_ns, features, ioParamBlock,
                                     getter_AddRefs(newWindow));

      NS_ENSURE_SUCCESS_LOG(rv, rv);

      rv = ioParamBlock->GetInt(0, &dialogReturn);
      if (NS_FAILED(rv) || dialogReturn == nsIToolkitProfileService::exit) {
        return NS_ERROR_ABORT;
      }

      int32_t startOffline;
      rv = ioParamBlock->GetInt(1, &startOffline);
      offline = NS_SUCCEEDED(rv) && startOffline == 1;

      rv = dlgArray->QueryElementAt(0, NS_GET_IID(nsIFile),
                                    getter_AddRefs(profD));
      NS_ENSURE_SUCCESS_LOG(rv, rv);

      rv = dlgArray->QueryElementAt(1, NS_GET_IID(nsIFile),
                                    getter_AddRefs(profLD));
      NS_ENSURE_SUCCESS_LOG(rv, rv);

      if (dialogReturn == nsIToolkitProfileService::launchWithProfile) {
        int32_t newArguments;
        rv = ioParamBlock->GetInt(2, &newArguments);

        if (NS_SUCCEEDED(rv) && newArguments > 0) {
          char** newArgv = (char**)realloc(
              gRestartArgv, sizeof(char*) * (gRestartArgc + newArguments + 1));
          NS_ENSURE_TRUE(newArgv, NS_ERROR_OUT_OF_MEMORY);

          gRestartArgv = newArgv;

          for (auto i = 0; i < newArguments; i++) {
            char16_t* arg;
            ioParamBlock->GetString(i, &arg);
            gRestartArgv[gRestartArgc++] =
                strdup(NS_ConvertUTF16toUTF8(nsDependentString(arg)).get());
          }

          gRestartArgv[gRestartArgc] = nullptr;
        }
      }
    }
  }

  if (offline) {
    SaveToEnv("XRE_START_OFFLINE=1");
  }

  if (dialogReturn == nsIToolkitProfileService::restart) {
    SaveToEnv("XRE_RESTART_TO_PROFILE_MANAGER=1");
  } else {
    MOZ_ASSERT(dialogReturn == nsIToolkitProfileService::launchWithProfile);
    SaveFileToEnv("XRE_PROFILE_PATH", profD);
    SaveFileToEnv("XRE_PROFILE_LOCAL_PATH", profLD);
  }
  if (gRestartedByOS) {
    char** newArgv =
        (char**)realloc(gRestartArgv, sizeof(char*) * (gRestartArgc + 2));
    NS_ENSURE_TRUE(newArgv, NS_ERROR_OUT_OF_MEMORY);
    gRestartArgv = newArgv;
    gRestartArgv[gRestartArgc++] = const_cast<char*>("-os-restarted");
    gRestartArgv[gRestartArgc] = nullptr;
  }

  return LaunchChild(false, true);
}

static ReturnAbortOnError ShowProfileManager(
    nsIToolkitProfileService* aProfileSvc, nsINativeAppSupport* aNative) {
  static const char kProfileManagerURL[] =
      "chrome://mozapps/content/profile/profileSelection.xhtml";
  return ShowProfileDialog(aProfileSvc, aNative, kProfileManagerURL);
}

static ReturnAbortOnError ShowProfileSelector(
    nsIToolkitProfileService* aProfileSvc, nsINativeAppSupport* aNative) {
  static const char kProfileSelectorURL[] = "about:profilemanager";
  return ShowProfileDialog(aProfileSvc, aNative, kProfileSelectorURL);
}

static nsresult LockProfile(nsINativeAppSupport* aNative, nsIFile* aRootDir,
                            nsIFile* aLocalDir, nsIToolkitProfile* aProfile,
                            nsIProfileLock** aResult) {

  static const int kLockRetrySeconds = 5;
  static const int kLockRetrySleepMS = 100;

  nsresult rv;
  nsCOMPtr<nsIProfileUnlocker> unlocker;
  const TimeStamp start = TimeStamp::Now();
  do {
    if (aProfile) {
      rv = aProfile->Lock(getter_AddRefs(unlocker), aResult);
    } else {
      rv = NS_LockProfilePath(aRootDir, aLocalDir, getter_AddRefs(unlocker),
                              aResult);
    }
    if (NS_SUCCEEDED(rv)) {
      StartupTimeline::Record(StartupTimeline::AFTER_PROFILE_LOCKED);
      return NS_OK;
    }
    PR_Sleep(kLockRetrySleepMS);
  } while (TimeStamp::Now() - start <
           TimeDuration::FromSeconds(kLockRetrySeconds));

  return ProfileLockedDialog(aRootDir, aLocalDir, unlocker, aNative, aResult);
}

static nsresult SelectProfile(nsToolkitProfileService* aProfileSvc,
                              nsINativeAppSupport* aNative, nsIFile** aRootDir,
                              nsIFile** aLocalDir, nsIToolkitProfile** aProfile,
                              bool* aWasDefaultSelection) {
  StartupTimeline::Record(StartupTimeline::SELECT_PROFILE);

  nsresult rv;

  if (EnvHasValue("XRE_RESTART_TO_PROFILE_MANAGER")) {
    return ShowProfileManager(aProfileSvc, aNative);
  }

  [[maybe_unused]] bool didCreate = false;
  rv = aProfileSvc->SelectStartupProfile(&gArgc, gArgv, false,
                                         aRootDir, aLocalDir, aProfile,
                                         &didCreate, aWasDefaultSelection);

  if (rv == NS_ERROR_SHOW_PROFILE_MANAGER) {
    return ShowProfileManager(aProfileSvc, aNative);
  }

  NS_ENSURE_SUCCESS(rv, rv);

  if (!*aRootDir) {
    NS_WARNING("Failed to select or create profile.");
    return NS_ERROR_ABORT;
  }

  return NS_OK;
}

#if defined(MOZ_BLOCK_PROFILE_DOWNGRADE)
static const char kProfileDowngradeURL[] =
    "chrome://mozapps/content/profile/profileDowngrade.xhtml";

static ReturnAbortOnError HandleDetectedDowngrade(
    nsIFile* aProfileDir, nsINativeAppSupport* aNative,
    nsIToolkitProfileService* aProfileSvc) {
  int32_t result = 0;
  nsresult rv;

  {
    ScopedXPCOMStartup xpcom;
    rv = xpcom.Initialize();
    NS_ENSURE_SUCCESS(rv, rv);

    rv = xpcom.SetWindowCreator(aNative);
    NS_ENSURE_SUCCESS(rv, rv);


    {  
      nsCOMPtr<nsIWindowWatcher> windowWatcher =
          do_GetService(NS_WINDOWWATCHER_CONTRACTID);
      NS_ENSURE_TRUE(windowWatcher, NS_ERROR_ABORT);

      nsCOMPtr<nsIAppStartup> appStartup(components::AppStartup::Service());
      NS_ENSURE_TRUE(appStartup, NS_ERROR_FAILURE);

      nsCOMPtr<nsIDialogParamBlock> paramBlock =
          do_CreateInstance(NS_DIALOGPARAMBLOCK_CONTRACTID);
      NS_ENSURE_TRUE(paramBlock, NS_ERROR_ABORT);

      paramBlock->SetInt(0, 0);

      nsAutoCString features("centerscreen,chrome,modal,titlebar");
      if (CheckArgExists("private-window") == ARG_FOUND) {
        features.AppendLiteral(",private");
      }
      nsCOMPtr<mozIDOMWindowProxy> newWindow;
      rv = windowWatcher->OpenWindow(
          nullptr, nsDependentCString(kProfileDowngradeURL), "_blank"_ns,
          features, paramBlock, getter_AddRefs(newWindow));
      NS_ENSURE_SUCCESS(rv, rv);

      paramBlock->GetInt(1, &result);

    }
  }

  if (result == nsIToolkitProfileService::createNewProfile) {
    nsCString profileName;
    profileName.AssignLiteral("default");
#if defined(MOZ_DEDICATED_PROFILES)
    profileName.Append("-" MOZ_STRINGIFY(MOZ_UPDATE_CHANNEL));
#endif
    nsCOMPtr<nsIToolkitProfile> newProfile;
    rv = aProfileSvc->CreateUniqueProfile(nullptr, profileName, "downgrade"_ns,
                                          getter_AddRefs(newProfile));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = aProfileSvc->SetDefaultProfile(newProfile);
    NS_ENSURE_SUCCESS(rv, rv);
    rv = aProfileSvc->Flush();
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIFile> profD, profLD;
    rv = newProfile->GetRootDir(getter_AddRefs(profD));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = newProfile->GetLocalDir(getter_AddRefs(profLD));
    NS_ENSURE_SUCCESS(rv, rv);

    SaveFileToEnv("XRE_PROFILE_PATH", profD);
    SaveFileToEnv("XRE_PROFILE_LOCAL_PATH", profLD);

    return LaunchChild(false, true);
  }

  return NS_ERROR_ABORT;
}
#endif

static void ExtractCompatVersionInfo(const nsACString& aCompatVersion,
                                     nsACString& aAppVersion,
                                     nsACString& aAppBuildID) {
  int32_t underscorePos = aCompatVersion.FindChar('_');
  int32_t slashPos = aCompatVersion.FindChar('/');

  if (underscorePos == kNotFound || slashPos == kNotFound ||
      slashPos < underscorePos) {
    NS_WARNING(
        "compatibility.ini Version string does not match the expected format.");

    aAppVersion = aCompatVersion;
    aAppBuildID.Truncate(0);
    return;
  }

  aAppVersion = Substring(aCompatVersion, 0, underscorePos);
  aAppBuildID = Substring(aCompatVersion, underscorePos + 1,
                          slashPos - (underscorePos + 1));
}

int32_t CompareCompatVersions(const nsACString& aOldCompatVersion,
                              const nsACString& aNewCompatVersion) {
  if (aOldCompatVersion.EqualsLiteral("Safe Mode")) {
    return -1;
  }

  int32_t index = aOldCompatVersion.FindChar('.');
  const nsACString& oldMajorVersion = Substring(
      aOldCompatVersion, 0, index < 0 ? aOldCompatVersion.Length() : index);
  index = aNewCompatVersion.FindChar('.');
  const nsACString& newMajorVersion = Substring(
      aNewCompatVersion, 0, index < 0 ? aNewCompatVersion.Length() : index);

  return CompareVersions(PromiseFlatCString(oldMajorVersion).get(),
                         PromiseFlatCString(newMajorVersion).get());
}

static bool CheckCompatibility(nsIFile* aProfileDir, const nsCString& aVersion,
                               const nsCString& aOSABI, nsIFile* aXULRunnerDir,
                               nsIFile* aAppDir, nsIFile* aFlagFile,
                               bool* aCachesOK, bool* aIsDowngrade,
                               nsCString& aLastVersion) {
  *aCachesOK = false;
  *aIsDowngrade = false;
  gLastAppVersion.SetIsVoid(true);
  gLastAppBuildID.SetIsVoid(true);
  gProfileEncryptedDatabases = false;

  nsCOMPtr<nsIFile> file;
  aProfileDir->Clone(getter_AddRefs(file));
  if (!file) return false;
  file->AppendNative(FILE_COMPATIBILITY_INFO);

  nsINIParser parser;
  nsresult rv = parser.Init(file);
  if (NS_FAILED(rv)) return false;

  {
    nsAutoCString encBuf;
    gProfileEncryptedDatabases =
        NS_SUCCEEDED(
            parser.GetString("Compatibility", "EncryptedDatabases", encBuf)) &&
        encBuf.EqualsLiteral("1");
  }

  rv = parser.GetString("Compatibility", "LastVersion", aLastVersion);
  if (NS_FAILED(rv)) {
    return false;
  }

  if (!aLastVersion.Equals(aVersion)) {
    *aIsDowngrade = 0 < CompareCompatVersions(aLastVersion, aVersion);
    ExtractCompatVersionInfo(aLastVersion, gLastAppVersion, gLastAppBuildID);
    return false;
  }


  gLastAppVersion.Assign(gAppData->version);
  gLastAppBuildID.Assign(gAppData->buildID);

  nsAutoCString buf;
  rv = parser.GetString("Compatibility", "LastOSABI", buf);
  if (NS_FAILED(rv) || !aOSABI.Equals(buf)) return false;

  rv = parser.GetString("Compatibility", "LastPlatformDir", buf);
  if (NS_FAILED(rv)) return false;

  nsCOMPtr<nsIFile> lf;
  rv = NS_NewLocalFileWithPersistentDescriptor(buf, getter_AddRefs(lf));
  if (NS_FAILED(rv)) return false;

  bool eq;
  rv = lf->Equals(aXULRunnerDir, &eq);
  if (NS_FAILED(rv) || !eq) return false;

  if (aAppDir) {
    rv = parser.GetString("Compatibility", "LastAppDir", buf);
    if (NS_FAILED(rv)) return false;

    rv = NS_NewLocalFileWithPersistentDescriptor(buf, getter_AddRefs(lf));
    if (NS_FAILED(rv)) return false;

    rv = lf->Equals(aAppDir, &eq);
    if (NS_FAILED(rv) || !eq) return false;
  }

  rv = parser.GetString("Compatibility", "InvalidateCaches", buf);
  *aCachesOK = (NS_FAILED(rv) || !buf.EqualsLiteral("1"));

  bool purgeCaches = false;
  if (aFlagFile && NS_SUCCEEDED(aFlagFile->Exists(&purgeCaches)) &&
      purgeCaches) {
    *aCachesOK = false;
  }

  return true;
}

void BuildCompatVersion(const char* aAppVersion, const char* aAppBuildID,
                        const char* aToolkitBuildID, nsACString& aBuf) {
  aBuf.Assign(aAppVersion);
  aBuf.Append('_');
  aBuf.Append(aAppBuildID);
  aBuf.Append('/');
  aBuf.Append(aToolkitBuildID);
}

static void BuildVersion(nsCString& aBuf) {
  BuildCompatVersion(gAppData->version, gAppData->buildID, gToolkitBuildID,
                     aBuf);
}

static void WriteVersion(nsIFile* aProfileDir, const nsCString& aVersion,
                         const nsCString& aOSABI, nsIFile* aXULRunnerDir,
                         nsIFile* aAppDir, bool invalidateCache) {
  nsCOMPtr<nsIFile> file;
  aProfileDir->Clone(getter_AddRefs(file));
  if (!file) return;
  file->AppendNative(FILE_COMPATIBILITY_INFO);

  nsAutoCString platformDir;
  (void)aXULRunnerDir->GetPersistentDescriptor(platformDir);

  nsAutoCString appDir;
  if (aAppDir) (void)aAppDir->GetPersistentDescriptor(appDir);

  PRFileDesc* fd;
  nsresult rv = file->OpenNSPRFileDesc(PR_WRONLY | PR_CREATE_FILE | PR_TRUNCATE,
                                       0600, &fd);
  if (NS_FAILED(rv)) {
    NS_ERROR("could not create output stream");
    return;
  }

  static const char kHeader[] = "[Compatibility]" NS_LINEBREAK "LastVersion=";

  PR_Write(fd, kHeader, sizeof(kHeader) - 1);
  PR_Write(fd, aVersion.get(), aVersion.Length());

  static const char kOSABIHeader[] = NS_LINEBREAK "LastOSABI=";
  PR_Write(fd, kOSABIHeader, sizeof(kOSABIHeader) - 1);
  PR_Write(fd, aOSABI.get(), aOSABI.Length());

  static const char kPlatformDirHeader[] = NS_LINEBREAK "LastPlatformDir=";

  PR_Write(fd, kPlatformDirHeader, sizeof(kPlatformDirHeader) - 1);
  PR_Write(fd, platformDir.get(), platformDir.Length());

  static const char kAppDirHeader[] = NS_LINEBREAK "LastAppDir=";
  if (aAppDir) {
    PR_Write(fd, kAppDirHeader, sizeof(kAppDirHeader) - 1);
    PR_Write(fd, appDir.get(), appDir.Length());
  }

  static const char kInvalidationHeader[] = NS_LINEBREAK "InvalidateCaches=1";
  if (invalidateCache)
    PR_Write(fd, kInvalidationHeader, sizeof(kInvalidationHeader) - 1);

  static const char kEncryptedHeader[] = NS_LINEBREAK "EncryptedDatabases=1";
  if (gProfileEncryptedDatabases)
    PR_Write(fd, kEncryptedHeader, sizeof(kEncryptedHeader) - 1);

  static const char kNL[] = NS_LINEBREAK;
  PR_Write(fd, kNL, sizeof(kNL) - 1);

  PR_Close(fd);
}

enum class DBHeaderResult { Encrypted, Plaintext, NoDBs };

static DBHeaderResult DetectEncryptedDBHeader(nsIFile* aProfileDir) {
  static const char* const kCandidates[] = {
      "cookies.sqlite", "places.sqlite",   "permissions.sqlite",
      "storage.sqlite", "favicons.sqlite",
  };

  for (const char* name : kCandidates) {
    nsCOMPtr<nsIFile> file;
    if (NS_FAILED(aProfileDir->Clone(getter_AddRefs(file))) || !file) continue;
    if (NS_FAILED(file->AppendNative(nsDependentCString(name)))) continue;

    bool exists = false;
    if (NS_FAILED(file->Exists(&exists)) || !exists) continue;

    PRFileDesc* fd = nullptr;
    if (NS_FAILED(file->OpenNSPRFileDesc(PR_RDONLY, 0, &fd)) || !fd) continue;

    static constexpr int32_t kSQLiteHeaderReadBytes = 24;
    static constexpr int32_t kSQLiteHeaderMinBytes = 21;
    static constexpr size_t kSQLitePageSizeOffset = 16;
    static constexpr size_t kSQLiteReservedOffset = 20;
    static constexpr uint16_t kObfsPageSize = 8192;
    static constexpr uint8_t kObfsReservedBytes = 32;

    uint8_t hdr[kSQLiteHeaderReadBytes];
    int32_t got = PR_Read(fd, hdr, sizeof(hdr));
    PR_Close(fd);
    if (got < kSQLiteHeaderMinBytes) continue;

    uint16_t pageSize = (uint16_t(hdr[kSQLitePageSizeOffset]) << 8) |
                        uint16_t(hdr[kSQLitePageSizeOffset + 1]);
    uint8_t reserved = hdr[kSQLiteReservedOffset];

    if (pageSize == kObfsPageSize && reserved == kObfsReservedBytes) {
      return DBHeaderResult::Encrypted;
    }
    return DBHeaderResult::Plaintext;
  }
  return DBHeaderResult::NoDBs;
}

enum class EncryptionCompatResult {
  OK,
  RefuseEncryptedButPrefOff,
  RefuseMigrationRequired,
};

static EncryptionCompatResult CheckEncryptionCompatibility(nsIFile* aProfileDir,
                                                           bool aPrefEnabled) {
  if (!aPrefEnabled) {
    if (gProfileEncryptedDatabases ||
        DetectEncryptedDBHeader(aProfileDir) == DBHeaderResult::Encrypted) {
      return EncryptionCompatResult::RefuseEncryptedButPrefOff;
    }
    return EncryptionCompatResult::OK;
  }

  if (!gProfileEncryptedDatabases &&
      DetectEncryptedDBHeader(aProfileDir) == DBHeaderResult::Plaintext) {
    return EncryptionCompatResult::RefuseMigrationRequired;
  }

  return EncryptionCompatResult::OK;
}

static bool RemoveComponentRegistries(nsIFile* aProfileDir,
                                      nsIFile* aLocalProfileDir,
                                      bool aRemoveEMFiles) {
  nsCOMPtr<nsIFile> file;
  aProfileDir->Clone(getter_AddRefs(file));
  if (!file) return false;

  if (aRemoveEMFiles) {
    file->SetNativeLeafName("extensions.ini"_ns);
    file->Remove(false);
  }

  aLocalProfileDir->Clone(getter_AddRefs(file));
  if (!file) return false;

  file->AppendNative("startupCache"_ns);
  nsresult rv = file->Remove(true);
  return NS_SUCCEEDED(rv) || rv == NS_ERROR_FILE_NOT_FOUND;
}

const XREAppData* gAppData = nullptr;

NS_VISIBILITY_DEFAULT PRBool nspr_use_zone_allocator = PR_FALSE;

#if defined(CAIRO_HAS_DWRITE_FONT)

#  include <dwrite.h>
#  include "nsWindowsHelpers.h"

#if defined(DEBUG_DWRITE_STARTUP)

#    define LOGREGISTRY(msg) LogRegistryEvent(msg)

static void LogRegistryEvent(const wchar_t* msg) {
  HKEY dummyKey;
  HRESULT hr;
  wchar_t buf[512];

  wsprintf(buf, L" log %s", msg);
  hr = RegOpenKeyEx(HKEY_LOCAL_MACHINE, buf, 0, KEY_READ, &dummyKey);
  if (SUCCEEDED(hr)) {
    RegCloseKey(dummyKey);
  }
}
#else

#    define LOGREGISTRY(msg)

#endif

static DWORD WINAPI InitDwriteBG(LPVOID lpdwThreadParam) {
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
  LOGREGISTRY(L"loading dwrite.dll");
  HMODULE dwdll = LoadLibrarySystem32(L"dwrite.dll");
  if (dwdll) {
    decltype(DWriteCreateFactory)* createDWriteFactory =
        (decltype(DWriteCreateFactory)*)GetProcAddress(dwdll,
                                                       "DWriteCreateFactory");
    if (createDWriteFactory) {
      LOGREGISTRY(L"creating dwrite factory");
      IDWriteFactory* factory;
      HRESULT hr = createDWriteFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&factory));
      if (SUCCEEDED(hr)) {
        LOGREGISTRY(L"dwrite factory done");
        factory->Release();
        LOGREGISTRY(L"freed factory");
      } else {
        LOGREGISTRY(L"failed to create factory");
      }
    }
  }
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
  return 0;
}
#endif
class XREMain {
 public:
  XREMain() = default;

  ~XREMain() {
    mScopedXPCOM = nullptr;
    mAppData = nullptr;
  }

  int XRE_main(int argc, char* argv[], const BootstrapConfig& aConfig);
  int XRE_mainInit(bool* aExitFlag);
  int XRE_mainStartup(bool* aExitFlag);
  nsresult XRE_mainRun();

  bool CheckLastStartupWasCrash();

  nsCOMPtr<nsINativeAppSupport> mNativeApp;
  RefPtr<nsToolkitProfileService> mProfileSvc;
  nsCOMPtr<nsIFile> mProfD;
  nsCOMPtr<nsIFile> mProfLD;
  nsCOMPtr<nsIProfileLock> mProfileLock;

  UniquePtr<ScopedXPCOMStartup> mScopedXPCOM;
  UniquePtr<XREAppData> mAppData;

  nsXREDirProvider mDirProvider;

#if defined(MOZ_WIDGET_GTK)
  nsAutoCString mXDGActivationToken;
  nsAutoCString mDesktopStartupID;
#endif

  bool mStartOffline = false;
  nsAutoCString mOriginToForceQUIC;
#if defined(MOZ_HAS_REMOTE)
  bool mDisableRemoteClient = false;
#endif
#if defined(MOZ_MINIMAL_BROWSER)
  bool mUsingEphemeralProfile = false;
#endif
};

#if defined(XP_UNIX) && !0
static SmprintfPointer FormatUid(uid_t aId) {
  if (const auto pw = getpwuid(aId)) {
    return mozilla::Smprintf("%s", pw->pw_name);
  }
  return mozilla::Smprintf("uid %d", static_cast<int>(aId));
}

static bool CheckForUserMismatch() {
  static char const* const kVars[] = {
      "HOME",
#if defined(MOZ_WIDGET_GTK)
      "XDG_RUNTIME_DIR",
#endif
  };

  const uid_t euid = geteuid();
  if (euid != 0) {
    return false;
  }

  for (const auto var : kVars) {
    if (const auto path = PR_GetEnv(var)) {
      struct stat st;
      if (stat(path, &st) == 0) {
        if (st.st_uid != euid) {
          const auto owner = FormatUid(st.st_uid);
          Output(true,
                 "Running " MOZ_APP_DISPLAYNAME
                 " as root in a regular"
                 " user's session is not supported.  ($%s is %s which is"
                 " owned by %s.)\n",
                 var, path, owner.get());
          return true;
        }
      }
    }
  }
  return false;
}
#else
static bool CheckForUserMismatch() { return false; }
#endif

void mozilla::startup::IncreaseDescriptorLimits() {
#if defined(XP_UNIX)
  static constexpr rlim_t kFDs = 65536;
  struct rlimit rlim;

  if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
    Output(false, "getrlimit: %s\n", strerror(errno));
    return;
  }
  if (rlim.rlim_cur != RLIM_INFINITY && rlim.rlim_cur < kFDs &&
      rlim.rlim_cur < rlim.rlim_max) {
    if (rlim.rlim_max != RLIM_INFINITY && rlim.rlim_max < kFDs) {
      rlim.rlim_cur = rlim.rlim_max;
    } else {
      rlim.rlim_cur = kFDs;
    }
    if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
      Output(false, "setrlimit: %s\n", strerror(errno));
    }
  }
#endif
}


#if defined(MOZ_BACKGROUNDTASKS)
static void SetupConsoleForBackgroundTask(
    const nsCString& aBackgroundTaskName) {
  if (BackgroundTasks::IsNoOutputTaskName(aBackgroundTaskName) &&
      !CheckArg("attach-console") &&
      !EnvHasValue("MOZ_BACKGROUNDTASKS_IGNORE_NO_OUTPUT")) {
    [[maybe_unused]] FILE* r0 = freopen("/dev/null", "w", stdout);
    [[maybe_unused]] FILE* r1 = freopen("/dev/null", "w", stderr);
    return;
  }
  printf_stderr("*** You are running in background task mode. ***\n");
}
#endif

int XREMain::XRE_mainInit(bool* aExitFlag) {
  if (!aExitFlag) return 1;
  *aExitFlag = false;

  atexit(UnexpectedExit);
  auto expectedShutdown = mozilla::MakeScopeExit([&] { MozExpectedExit(); });

  StartupTimeline::Record(StartupTimeline::MAIN);

  nsresult rv = NS_CreateNativeAppSupport(getter_AddRefs(mNativeApp));
  if (NS_FAILED(rv)) return 1;

  if (CheckArg("v") || CheckArg("version")) {
    DumpVersion();
    *aExitFlag = true;
    return 0;
  }

  if (CheckArg("full-version")) {
    DumpFullVersion();
    *aExitFlag = true;
    return 0;
  }

  if (CheckForUserMismatch()) {
    return 1;
  }


#if defined(MOZ_BACKGROUNDTASKS)
  Maybe<nsCString> backgroundTask = Nothing();
  const char* backgroundTaskName = nullptr;
  if (ARG_FOUND ==
      CheckArg("backgroundtask", &backgroundTaskName, CheckArgFlag::None)) {
    backgroundTask = Some(backgroundTaskName);
    SetupConsoleForBackgroundTask(backgroundTask.ref());
  }

  BackgroundTasks::Init(backgroundTask);
#endif

  if (auto* featureStr = PR_GetEnv("MOZ_CHAOSMODE")) {
    ChaosFeature feature = ChaosFeature::Any;
    long featureInt = strtol(featureStr, nullptr, 16);
    if (featureInt) {
      feature = static_cast<ChaosFeature>(featureInt);
    }
    ChaosMode::SetChaosFeature(feature);
    ChaosMode::enterChaosMode();
    MOZ_ASSERT(ChaosMode::isActive(ChaosFeature::Any));
  }

  if (CheckArgExists("fxr")) {
    gFxREmbedded = true;
  }

  if (ChaosMode::isActive(ChaosFeature::Any)) {
    printf_stderr(
        "*** You are running in chaos test mode. See ChaosMode.h. ***\n");
  }

  gKioskMode = CheckArg("kiosk", nullptr, CheckArgFlag::None);
  const char* kioskMonitorNumber = nullptr;
  if (CheckArg("kiosk-monitor", &kioskMonitorNumber, CheckArgFlag::None)) {
    gKioskMode = true;
    gKioskMonitor = atoi(kioskMonitorNumber);
  }

  ArgResult ar;

#if defined(DEBUG)
  if (PR_GetEnv("XRE_MAIN_BREAK")) NS_BREAK();
#endif

  mozilla::startup::IncreaseDescriptorLimits();

  SetupErrorHandling(gArgv[0]);

#if defined(CAIRO_HAS_DWRITE_FONT)
  {

    CreateThread(nullptr, 0, &InitDwriteBG, nullptr, 0, nullptr);
  }
#endif

#if defined(XP_UNIX)
  const char* home = PR_GetEnv("HOME");
  if (!home || !*home) {
    struct passwd* pw = getpwuid(geteuid());
    if (!pw || !pw->pw_dir) {
      Output(true, "Could not determine HOME directory");
      return 1;
    }
    SaveWordToEnv("HOME", nsDependentCString(pw->pw_dir));
  }
#endif

#if defined(MOZ_ACCESSIBILITY_ATK)
  SaveToEnv("NO_AT_BRIDGE=1");
#endif

  const char* override = nullptr;
  ar = CheckArg("override", &override);
  if (ar == ARG_BAD) {
    Output(true, "Incorrect number of arguments passed to --override");
    return 1;
  }
  if (ar == ARG_FOUND) {
    nsCOMPtr<nsIFile> overrideLF;
    rv = XRE_GetFileFromPath(override, getter_AddRefs(overrideLF));
    if (NS_FAILED(rv)) {
      Output(true, "Error: unrecognized override.ini path.\n");
      return 1;
    }

    rv = XRE_ParseAppData(overrideLF, *mAppData);
    if (NS_FAILED(rv)) {
      Output(true, "Couldn't read override.ini");
      return 1;
    }
  }


  if (!mAppData->name) {
    Output(true, "Error: App:Name not specified in application.ini\n");
    return 1;
  }
  if (!mAppData->buildID) {
    Output(true, "Error: App:BuildID not specified in application.ini\n");
    return 1;
  }


  if (!mAppData->minVersion) {
    Output(true, "Error: Gecko:MinVersion not specified in application.ini\n");
    return 1;
  }

  if (!mAppData->maxVersion) {
    mAppData->maxVersion = "1.*";
  }

  if (mozilla::Version(mAppData->minVersion) > gToolkitVersion ||
      mozilla::Version(mAppData->maxVersion) < gToolkitVersion) {
    Output(true,
           "Error: Platform version '%s' is not compatible with\n"
           "minVersion >= %s\nmaxVersion <= %s\n"
           "Maybe try to reinstall " MOZ_APP_DISPLAYNAME "?\n",
           (const char*)gToolkitVersion, (const char*)mAppData->minVersion,
           (const char*)mAppData->maxVersion);
    return 1;
  }

  rv = mDirProvider.Initialize(mAppData->directory, mAppData->xreDirectory);
  if (NS_FAILED(rv)) return 1;



  SaveToEnv("MOZ_LAUNCHED_CHILD=");

  if (CheckArg("os-restarted", nullptr, CheckArgFlag::RemoveArg) == ARG_FOUND) {
    gRestartedByOS = true;
  }

  gRestartArgc = gArgc;
  gRestartArgv =
      (char**)malloc(sizeof(char*) * (gArgc + 1 + (override ? 2 : 0)));
  if (!gRestartArgv) {
    return 1;
  }

  int i;
  for (i = 0; i < gArgc; ++i) {
    gRestartArgv[i] = gArgv[i];
  }

  if (override) {
    gRestartArgv[gRestartArgc++] = const_cast<char*>("-override");
    gRestartArgv[gRestartArgc++] = const_cast<char*>(override);
  }

  gRestartArgv[gRestartArgc] = nullptr;

  Maybe<bool> safeModeRequested = IsSafeModeRequested(gArgc, gArgv);
  if (!safeModeRequested) {
    return 1;
  }
#if defined(MOZ_BACKGROUNDTASKS)
  if (BackgroundTasks::IsBackgroundTaskMode()) {
    safeModeRequested = Some(false);

    const char* tmpBackgroundTaskName = nullptr;
    (void)CheckArg("backgroundtask", &tmpBackgroundTaskName,
                   CheckArgFlag::RemoveArg);
  }
#endif

  gSafeMode = safeModeRequested.value();

  CheckArg("no-remote");

#if defined(MOZ_HAS_REMOTE)
  ar = CheckArg("new-instance");
  if (ar == ARG_FOUND || EnvHasValue("MOZ_NEW_INSTANCE")) {
    mDisableRemoteClient = true;
  }
#else
  CheckArg("new-instance");
#endif

  CheckArg("wait-for-browser");

  ar = CheckArg("offline");
  if (ar || EnvHasValue("XRE_START_OFFLINE")) {
    mStartOffline = true;
  }

  const char* origin = nullptr;
  if (!PR_GetEnv("MOZ_FORCE_QUIC_ON") &&
      ARG_FOUND == CheckArg("origin-to-force-quic-on", &origin,
                            CheckArgFlag::RemoveArg)) {
    mOriginToForceQUIC.Assign(origin);
  }

  if (CheckArg("h") || CheckArg("help") || CheckArg("?")) {
    DumpHelp();
    *aExitFlag = true;
    return 0;
  }

#if defined(MOZ_ENABLE_DBUS)
  const char* dbusServiceLauncher = nullptr;
  ar = CheckArg("dbus-service", &dbusServiceLauncher, CheckArgFlag::None);
  if (ar == ARG_BAD) {
    Output(true, "Missing launcher param for --dbus-service\n");
    return 1;
  }
  if (ar == ARG_FOUND) {
    UniquePtr<DBusService> dbusService =
        MakeUnique<DBusService>(dbusServiceLauncher);
    if (dbusService->Init()) {
      dbusService->Run();
    }
    *aExitFlag = true;
    return 0;
  }

#if defined(MOZ_WIDGET_GTK)
  if (XRE_IsParentProcess()) {
    widget::RegisterHostApp();
  }
#endif
#endif

  rv = XRE_InitCommandLine(gArgc, gArgv);
  NS_ENSURE_SUCCESS(rv, 1);

  return 0;
}




#if defined(MOZ_UPDATER) && !0
enum struct ShouldNotProcessUpdatesReason {
  NotAnUpdatingTask,
  OtherInstanceRunning,
  FirstStartup
};

const char* ShouldNotProcessUpdatesReasonAsString(
    ShouldNotProcessUpdatesReason aReason) {
  switch (aReason) {
    case ShouldNotProcessUpdatesReason::NotAnUpdatingTask:
      return "NotAnUpdatingTask";
    case ShouldNotProcessUpdatesReason::OtherInstanceRunning:
      return "OtherInstanceRunning";
    default:
      MOZ_CRASH("impossible value for ShouldNotProcessUpdatesReason");
  }
}

Maybe<ShouldNotProcessUpdatesReason> ShouldNotProcessUpdates(
    nsXREDirProvider& aDirProvider) {
  if (ARG_FOUND == CheckArgExists("first-startup")) {
    NS_WARNING("ShouldNotProcessUpdates(): FirstStartup");
    return Some(ShouldNotProcessUpdatesReason::FirstStartup);
  }

#if defined(MOZ_BACKGROUNDTASKS)
  Maybe<nsCString> backgroundTasks = BackgroundTasks::GetBackgroundTasks();
  if (backgroundTasks.isSome()) {
    if (!BackgroundTasks::IsUpdatingTaskName(backgroundTasks.ref())) {
      NS_WARNING("ShouldNotProcessUpdates(): NotAnUpdatingTask");
      return Some(ShouldNotProcessUpdatesReason::NotAnUpdatingTask);
    }

    nsCOMPtr<nsIFile> anAppFile;
    bool persistent;
    nsresult rv = aDirProvider.GetFile(XRE_EXECUTABLE_FILE, &persistent,
                                       getter_AddRefs(anAppFile));
    if (NS_FAILED(rv) || !anAppFile) {
      return Nothing();
    }

    auto updateSyncManager = new nsUpdateSyncManager(anAppFile);

    bool otherInstance = false;
    updateSyncManager->IsOtherInstanceRunning(&otherInstance);
    if (otherInstance) {
      NS_WARNING("ShouldNotProcessUpdates(): OtherInstanceRunning");
      return Some(ShouldNotProcessUpdatesReason::OtherInstanceRunning);
    }
  }
#endif

  return Nothing();
}
#endif

namespace mozilla::startup {
Result<nsCOMPtr<nsIFile>, nsresult> GetIncompleteStartupFile(nsIFile* aProfLD) {
  nsCOMPtr<nsIFile> crashFile;
  MOZ_TRY(aProfLD->Clone(getter_AddRefs(crashFile)));
  MOZ_TRY(crashFile->Append(FILE_STARTUP_INCOMPLETE));
  return std::move(crashFile);
}
}  

bool XREMain::CheckLastStartupWasCrash() {
  Result<nsCOMPtr<nsIFile>, nsresult> crashFile =
      GetIncompleteStartupFile(mProfLD);
  if (crashFile.isErr()) {
    return true;
  }

  AutoFDClose fd;
  (void)crashFile.inspect()->OpenNSPRFileDesc(
      PR_WRONLY | PR_CREATE_FILE | PR_EXCL, 0666, getter_Transfers(fd));
  return !fd;
}

int XREMain::XRE_mainStartup(bool* aExitFlag) {
  nsresult rv;

  if (!aExitFlag) return 1;
  *aExitFlag = false;



#if defined(MOZ_WIDGET_GTK)
  if (const char* v = PR_GetEnv("DESKTOP_STARTUP_ID")) {
    mDesktopStartupID.Assign(v);
  }
  if (const char* v = PR_GetEnv("XDG_ACTIVATION_TOKEN")) {
    mXDGActivationToken.Assign(v);
  }
#endif


#if defined(MOZ_WIDGET_GTK)

  g_set_prgname(gAppData->remotingName);



  if (!gtk_parse_args(&gArgc, &gArgv)) return 1;
#endif


  bool isBackgroundTaskMode = false;
#if defined(MOZ_BACKGROUNDTASKS)
  isBackgroundTaskMode = BackgroundTasks::IsBackgroundTaskMode();
  if (isBackgroundTaskMode) {
  }
#endif

#if defined(MOZ_HAS_REMOTE)
#if defined(MOZ_MINIMAL_BROWSER)
  mDisableRemoteClient = true;
#endif
#endif

#if defined(MOZ_WIDGET_GTK)
  if (!isBackgroundTaskMode) {
    const char* display_name = nullptr;
    bool saveDisplayArg = false;

    display_name = gdk_get_display_arg_name();
    bool waylandEnabled = IsWaylandEnabled();
#if defined(MOZ_WAYLAND)
    if (!display_name) {
      auto* proxyEnv = getenv("MOZ_DISABLE_WAYLAND_PROXY");
      bool disableWaylandProxy = proxyEnv && *proxyEnv;
      if (!disableWaylandProxy && XRE_IsParentProcess() && waylandEnabled) {
        auto* proxyLog = getenv("WAYLAND_PROXY_LOG");
        WaylandProxy::SetVerbose(proxyLog && *proxyLog);
        WaylandProxy::SetCompositorUnavailableHandler(
            WlCompositorUnavailableHandler);
        WaylandProxy::SetCompositorSilentDisconnectHandler(
            WlCompositorSilentDisconnectHandler);
        WaylandProxy::AddState(WAYLAND_PROXY_ENABLED);
        gWaylandProxy = WaylandProxy::Create();
        if (gWaylandProxy) {
          if (!gWaylandProxy->RunThread()) {
            Output(true, "Failed to run Wayland proxy\n");
          }
        }
      } else {
        WaylandProxy::AddState(WAYLAND_PROXY_DISABLED);
      }
    }
#endif

    if (display_name) {
      SaveWordToEnv("MOZ_GDK_DISPLAY", nsDependentCString(display_name));
      saveDisplayArg = true;
    }

    if (!waylandEnabled && !display_name) {
      display_name = PR_GetEnv("DISPLAY");
      if (!display_name) {
        PR_fprintf(PR_STDERR,
                   "Error: no DISPLAY environment variable specified\n");
        return 1;
      }
    }

    if (display_name) {
      GdkDisplay* disp = gdk_display_open(display_name);
      if (!disp) {
        PR_fprintf(PR_STDERR, "Error: cannot open display: %s\n", display_name);
        return 1;
      }
      if (saveDisplayArg) {
        if (GdkIsX11Display(disp)) {
          SaveWordToEnv("DISPLAY", nsDependentCString(display_name));
        } else if (GdkIsWaylandDisplay(disp)) {
          SaveWordToEnv("WAYLAND_DISPLAY", nsDependentCString(display_name));
        }
      }
    } else {
      gdk_display_manager_open_display(gdk_display_manager_get(), nullptr);
    }
#if defined(MOZ_WAYLAND)
    if (gWaylandProxy) {
      gWaylandProxy->RestoreWaylandDisplay();
    }
    if (waylandEnabled && PR_GetEnv("WAYLAND_DISPLAY") && GdkIsX11Display()) {
      Output(true,
             "Error: Failed to open Wayland display, fallback to X11. "
             "WAYLAND_DISPLAY='%s' DISPLAY='%s'\n",
             PR_GetEnv("WAYLAND_DISPLAY"), PR_GetEnv("DISPLAY"));

      g_unsetenv("WAYLAND_DISPLAY");
      gWaylandProxy = nullptr;
    }
#endif
    if (!gdk_display_get_default()) {
      Output(true,
             "Error: we don't have any display, WAYLAND_DISPLAY='%s' "
             "DISPLAY='%s'\n",
             PR_GetEnv("WAYLAND_DISPLAY"), PR_GetEnv("DISPLAY"));
      return 1;
    }
#if defined(MOZ_WAYLAND)
    if (!GdkIsWaylandDisplay()) {
      Output(true, "Wayland only build is missig Wayland display!\n");
      return 1;
    }
#endif
#if !defined(MOZ_WAYLAND)
    if (!GdkIsX11Display()) {
      Output(true, "X11 only build is missig X11 display!\n");
      return 1;
    }
#endif
  }
#endif
#if defined(MOZ_HAS_REMOTE)
  gRemoteService = new nsRemoteService();
  if (gRemoteService) {
    gRemoteService->SetProgram(gAppData->remotingName);
    gStartupLock = gRemoteService->LockStartup();
    if (!gStartupLock) {
      NS_WARNING("Failed to lock for startup, continuing anyway.");
    }
  }
#endif
#if defined(MOZ_WIDGET_GTK)
  g_set_application_name(mAppData->name);

#endif

  bool canRun = false;
  rv = mNativeApp->Start(&canRun);
  if (NS_FAILED(rv) || !canRun) {
    return 1;
  }

#if defined(MOZ_WIDGET_GTK)
  if (!mDesktopStartupID.IsEmpty()) {
    PR_SetEnv(ToNewCString("DESKTOP_STARTUP_ID="_ns + mDesktopStartupID));
  }

  if (!mXDGActivationToken.IsEmpty()) {
    PR_SetEnv(ToNewCString("XDG_ACTIVATION_TOKEN="_ns + mXDGActivationToken));
  }
#endif

  if (CheckArg("test-launch-without-hang")) {
    *aExitFlag = true;
    return 0;
  }

  mProfileSvc = NS_GetToolkitProfileService();
  if (!mProfileSvc) {
    ProfileMissingDialog(mNativeApp);
    return 1;
  }

  bool wasDefaultSelection;
  nsCOMPtr<nsIToolkitProfile> profile;
  rv = SelectProfile(mProfileSvc, mNativeApp, getter_AddRefs(mProfD),
                     getter_AddRefs(mProfLD), getter_AddRefs(profile),
                     &wasDefaultSelection);
  if (rv == NS_ERROR_LAUNCHED_CHILD_PROCESS || rv == NS_ERROR_ABORT) {
    *aExitFlag = true;
    return 0;
  }

  if (NS_FAILED(rv)) {
    ProfileMissingDialog(mNativeApp);
    return 1;
  }

#if defined(MOZ_MINIMAL_BROWSER)
  mUsingEphemeralProfile = true;
#endif

#if defined(MOZ_HAS_REMOTE)
  if (gRemoteService) {
    nsCString profilePath;

    rv = mProfD->GetNativePath(profilePath);

    if (NS_SUCCEEDED(rv)) {
      gRemoteService->SetProfile(profilePath);

      if (!mDisableRemoteClient) {
#if defined(MOZ_WIDGET_GTK)
        auto& startupToken =
            GdkIsWaylandDisplay() ? mXDGActivationToken : mDesktopStartupID;
        gRemoteService->SetStartupToken(startupToken);
#endif
        rv = gRemoteService->StartClient();

        if (rv == NS_ERROR_NOT_AVAILABLE && profile) {
          nsCString profileName;
          profile->GetName(profileName);
          gRemoteService->SetProfile(profileName);

          rv = gRemoteService->StartClient();

          gRemoteService->SetProfile(profilePath);
        }

        if (NS_SUCCEEDED(rv)) {
          *aExitFlag = true;
          gStartupLock = nullptr;
          return 0;
        }

        if (rv == NS_ERROR_INVALID_ARG) {
          gStartupLock = nullptr;
          return 1;
        }
      }
    }
  }
#endif

#if defined(MOZ_UPDATER) && !0
  nsCOMPtr<nsIFile> updRoot;
  bool persistent;
  rv = mDirProvider.GetFile(XRE_UPDATE_ROOT_DIR, &persistent,
                            getter_AddRefs(updRoot));
  if (NS_FAILED(rv)) {
    updRoot = mDirProvider.GetAppDir();
  }

  Maybe<ShouldNotProcessUpdatesReason> shouldNotProcessUpdatesReason =
      ShouldNotProcessUpdates(mDirProvider);
  if (shouldNotProcessUpdatesReason.isNothing()) {
    nsCOMPtr<nsIFile> exeFile, exeDir;
    rv = mDirProvider.GetFile(XRE_EXECUTABLE_FILE, &persistent,
                              getter_AddRefs(exeFile));
    NS_ENSURE_SUCCESS(rv, 1);
    rv = exeFile->GetParent(getter_AddRefs(exeDir));
    NS_ENSURE_SUCCESS(rv, 1);
    ProcessUpdates(mDirProvider.GetGREDir(), exeDir, updRoot, gRestartArgc,
                   gRestartArgv, mAppData->version);
  } else {
    if (CheckArg("test-should-not-process-updates") ||
        EnvHasValue("MOZ_TEST_SHOULD_NOT_PROCESS_UPDATES")) {

      SaveToEnv(nsPrintfCString("MOZ_TEST_SHOULD_NOT_PROCESS_UPDATES="
                                "ShouldNotProcessUpdates(): %s",
                                ShouldNotProcessUpdatesReasonAsString(
                                    shouldNotProcessUpdatesReason.value()))
                    .get());
    }
  }
  if (CheckArg("test-process-updates") ||
      EnvHasValue("MOZ_TEST_PROCESS_UPDATES")) {
    SaveToEnv("MOZ_TEST_PROCESS_UPDATES=");

    const char* logFile = nullptr;
    if (ARG_FOUND == CheckArg("dump-args", &logFile)) {
      FILE* logFP = fopen(logFile, "wb");
      if (logFP) {
        for (int i = 1; i < gRestartArgc; ++i) {
          fprintf(logFP, "%s\n", gRestartArgv[i]);
        }
        fclose(logFP);
      }
    }

    WriteUpdateCompleteTestFile(updRoot);

    *aExitFlag = true;
    return 0;
  }
#endif


  if (wasDefaultSelection) {
    if (!mProfileSvc->GetStartWithLastProfile()) {
      rv = ShowProfileManager(mProfileSvc, mNativeApp);
    } else if (profile && profile->GetShowProfileSelector()) {
      rv = ShowProfileSelector(mProfileSvc, mNativeApp);
    } else {
      rv = NS_OK;
    }

    if (rv == NS_ERROR_LAUNCHED_CHILD_PROCESS || rv == NS_ERROR_ABORT) {
      *aExitFlag = true;
      return 0;
    }

    if (NS_FAILED(rv)) {
      return 1;
    }
  }

  rv = LockProfile(mNativeApp, mProfD, mProfLD, profile,
                   getter_AddRefs(mProfileLock));
  if (rv == NS_ERROR_LAUNCHED_CHILD_PROCESS || rv == NS_ERROR_ABORT) {
    *aExitFlag = true;
    return 0;
  }
  if (NS_FAILED(rv)) {
    return 1;
  }

  gProfileLock = mProfileLock;

  nsAutoCString version;
  BuildVersion(version);

#if defined(TARGET_OS_ABI)
  constexpr auto osABI = nsLiteralCString{TARGET_OS_ABI};
#else
  constexpr auto osABI = nsLiteralCString{OS_TARGET "_UNKNOWN"};
#endif


  nsCOMPtr<nsIFile> flagFile;
  if (mAppData->directory) {
    (void)mAppData->directory->Clone(getter_AddRefs(flagFile));
  }
  if (flagFile) {
    flagFile->AppendNative(FILE_INVALIDATE_CACHES);
  }

  bool cachesOK;
  bool isDowngrade;
  nsCString lastVersion;
  bool versionOK = CheckCompatibility(
      mProfD, version, osABI, mDirProvider.GetGREDir(), mAppData->directory,
      flagFile, &cachesOK, &isDowngrade, lastVersion);

  MOZ_RELEASE_ASSERT(!cachesOK || lastVersion.Equals(version),
                     "Caches cannot be good if the version has changed.");

#if defined(MOZ_BACKGROUNDTASKS)
  if (!BackgroundTasks::IsBackgroundTaskMode())
#endif
  {
    bool prefEnabled =
        StaticPrefs::security_storage_encryption_sqlite_enabled();
    EncryptionCompatResult ec =
        CheckEncryptionCompatibility(mProfD, prefEnabled);
    if (ec != EncryptionCompatResult::OK) {
      const char* msgKey =
          ec == EncryptionCompatResult::RefuseEncryptedButPrefOff
              ? "profileEncryptedButPrefOff"
              : "profileNotEncryptedButPrefOn";
      const char* titleKey =
          ec == EncryptionCompatResult::RefuseEncryptedButPrefOff
              ? "profileEncryptedButPrefOffTitle"
              : "profileNotEncryptedButPrefOnTitle";
      (void)ProfileEncryptionMismatchDialog(msgKey, titleKey, mNativeApp);
      *aExitFlag = true;
      return 0;
    }
  }

#if defined(MOZ_BLOCK_PROFILE_DOWNGRADE)
  if (!CheckArg("allow-downgrade") && isDowngrade &&
      !EnvHasValue("MOZ_ALLOW_DOWNGRADE")) {
    rv = HandleDetectedDowngrade(mProfD, mNativeApp, mProfileSvc);
    if (rv == NS_ERROR_LAUNCHED_CHILD_PROCESS || rv == NS_ERROR_ABORT) {
      *aExitFlag = true;
      return 0;
    }
  }
#endif

  rv = mDirProvider.SetProfile(mProfD, mProfLD);
  NS_ENSURE_SUCCESS(rv, 1);



  bool lastStartupWasCrash = CheckLastStartupWasCrash();

  if (CheckArg("purgecaches") || PR_GetEnv("MOZ_PURGE_CACHES") ||
      lastStartupWasCrash || gSafeMode) {
    cachesOK = false;
  }

  bool startupCacheValid = true;

  if (!cachesOK || !versionOK) {
    QuotaManager::InvalidateQuotaCache();

    startupCacheValid = RemoveComponentRegistries(mProfD, mProfLD, false);

    WriteVersion(mProfD, version, osABI, mDirProvider.GetGREDir(),
                 mAppData->directory, gSafeMode || !startupCacheValid);
  }

  if (!startupCacheValid) StartupCache::IgnoreDiskCache();

  if (flagFile) {
    flagFile->Remove(true);
  }

  if (!isBackgroundTaskMode) {
#if defined(USE_GLX_TEST)
    GfxInfo::FireGLXTestProcess();
#endif
#if defined(MOZ_WAYLAND)
    if (IsWaylandEnabled()) {
      (void)WaylandDisplayGet();
    }
#endif
#if defined(MOZ_WIDGET_GTK)
    nsAppShell::InstallTermSignalHandler();
#endif
  }

  return 0;
}

nsresult XREMain::XRE_mainRun() {
  nsresult rv = NS_OK;
  NS_ASSERTION(mScopedXPCOM, "Scoped xpcom not initialized.");


  nsCOMPtr<nsIAppStartup> appStartup;
  nsCOMPtr<nsICommandLineRunner> cmdLine;

  {

    rv = mScopedXPCOM->SetWindowCreator(mNativeApp);
    NS_ENSURE_SUCCESS(rv, NS_ERROR_FAILURE);

    if (mStartOffline) {
      nsCOMPtr<nsIIOService> io(
          do_GetService("@mozilla.org/network/io-service;1"));
      NS_ENSURE_TRUE(io, NS_ERROR_FAILURE);
      io->SetManageOfflineStatus(false);
      io->SetOffline(true);
    }

    if (!mOriginToForceQUIC.IsEmpty()) {
      PR_SetEnv(ToNewCString("MOZ_FORCE_QUIC_ON="_ns + mOriginToForceQUIC));
    }


    mDirProvider.InitializeUserPrefs();

    mProfileSvc->UpdateCurrentProfile();

    xpc::InitializeJSContext();

    mDirProvider.FinishInitializingUserPrefs();

    {
      mozilla::dom::AutoJSAPI jsapi;
      MOZ_ALWAYS_TRUE(jsapi.Init(xpc::PrivilegedJunkScope()));
      JS::Rooted<JSObject*> mod(jsapi.cx());
      rv = mozJSModuleLoader::Get()->ImportESModule(
          jsapi.cx(), "resource://gre/modules/AppConstants.sys.mjs"_ns, &mod);
      if (NS_FAILED(rv)) {
        return NS_ERROR_OMNIJAR_OR_DIR_MISSING;
      }
    }


    nsCOMPtr<nsIFile> workingDir;
    rv = NS_GetSpecialDirectory(NS_OS_CURRENT_WORKING_DIR,
                                getter_AddRefs(workingDir));
    if (NS_FAILED(rv)) {
      workingDir = nullptr;
    }

    cmdLine = new nsCommandLine();

    rv = cmdLine->Init(gArgc, gArgv, workingDir,
                       nsICommandLine::STATE_INITIAL_LAUNCH);
    NS_ENSURE_SUCCESS(rv, NS_ERROR_FAILURE);

    NS_CreateServicesFromCategory("app-startup", cmdLine, "app-startup",
                                  nullptr);

    appStartup = components::AppStartup::Service();
    NS_ENSURE_TRUE(appStartup, NS_ERROR_FAILURE);

    mDirProvider.DoStartup();


    mozilla::FilePreferences::InitDirectoriesAllowlist();
    mozilla::FilePreferences::InitPrefs();

    if (!AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
      nsCOMPtr<nsIObserverService> obsService =
          mozilla::services::GetObserverService();
      if (obsService) {
        obsService->NotifyObservers(cmdLine, "command-line-startup", nullptr);
      }
    }


    mozilla::AppShutdown::SaveEnvVarsForPotentialRestart();

    SaveToEnv("XRE_PROFILE_PATH=");
    SaveToEnv("XRE_PROFILE_LOCAL_PATH=");
    SaveToEnv("XRE_START_OFFLINE=");
    SaveToEnv("XUL_APP_FILE=");
    SaveToEnv("XRE_BINARY_PATH=");
    SaveToEnv("XRE_RESTARTED_BY_PROFILE_MANAGER=");

    if (!AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {


#if defined(MOZ_WIDGET_GTK)
      for (const auto& name : kStartupTokenNames) {
        g_unsetenv(name.get());
      }
      nsAppShell::InitSessionRestore();
#endif


      nsCOMPtr<nsIObserverService> obsService =
          mozilla::services::GetObserverService();
      if (obsService)
        obsService->NotifyObservers(nullptr, "final-ui-startup", nullptr);

      (void)appStartup->DoneStartingUp();

      AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed);
    }

    if (!AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
      rv = cmdLine->Run();
      NS_ENSURE_SUCCESS_LOG(rv, NS_ERROR_FAILURE);
    }

    if (!AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
#if defined(MOZ_HAS_REMOTE)
      if (gRemoteService) {
        gRemoteService->StartupServer();
        gStartupLock = nullptr;
      }
#endif

      mNativeApp->Enable();
    }

    mProfileSvc->CompleteStartup();
  }

#if defined(MOZ_BACKGROUNDTASKS)
  if (BackgroundTasks::IsBackgroundTaskMode()) {
    rv = appStartup->TrackStartupCrashEnd();
    NS_ENSURE_SUCCESS(rv, rv);

    rv = appStartup->EnterLastWindowClosingSurvivalArea();
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIPowerManagerService> powerManagerService =
        do_GetService(POWERMANAGERSERVICE_CONTRACTID);
    nsCOMPtr<nsIStringBundleService> stringBundleService =
        do_GetService(NS_STRINGBUNDLE_CONTRACTID);

    rv = BackgroundTasks::RunBackgroundTask(cmdLine);
    NS_ENSURE_SUCCESS(rv, rv);
  }
#endif

  cmdLine = nullptr;

  {
    rv = appStartup->Run();
    if (NS_FAILED(rv)) {
      NS_ERROR("failed to run appstartup");
      gLogConsoleErrors = true;
    }
  }

  return rv;
}


int XREMain::XRE_main(int argc, char* argv[], const BootstrapConfig& aConfig) {
  gArgc = argc;
  gArgv = argv;

  ScopedLogging log;

  mozilla::LogModule::Init(gArgc, gArgv);





#if defined(MOZ_CODE_COVERAGE)
  CodeCoverageHandler::Init();
#endif

  nsresult rv = NS_OK;

  if (aConfig.appData) {
    mAppData = MakeUnique<XREAppData>(*aConfig.appData);
  } else {
    MOZ_RELEASE_ASSERT(aConfig.appDataPath);
    nsCOMPtr<nsIFile> appini;
    rv = XRE_GetFileFromPath(aConfig.appDataPath, getter_AddRefs(appini));
    if (NS_FAILED(rv)) {
      Output(true, "Error: unrecognized path: %s\n", aConfig.appDataPath);
      return 1;
    }

    mAppData = MakeUnique<XREAppData>();
    rv = XRE_ParseAppData(appini, *mAppData);
    if (NS_FAILED(rv)) {
      Output(true, "Couldn't read application.ini");
      return 1;
    }

    appini->GetParent(getter_AddRefs(mAppData->directory));
  }

  const char* appRemotingName = getenv("MOZ_APP_REMOTINGNAME");
  if (appRemotingName) {
    mAppData->remotingName = appRemotingName;
  } else if (!mAppData->remotingName) {
    mAppData->remotingName = mAppData->name;
  }
  gAppData = mAppData.get();

  nsCOMPtr<nsIFile> binFile;
  rv = XRE_GetBinaryPath(getter_AddRefs(binFile));
  NS_ENSURE_SUCCESS(rv, 1);

  rv = binFile->GetPath(gAbsoluteArgv0Path);
  NS_ENSURE_SUCCESS(rv, 1);

  if (!mAppData->xreDirectory) {
    nsCOMPtr<nsIFile> greDir;

    rv = binFile->GetParent(getter_AddRefs(greDir));
    if (NS_FAILED(rv)) return 2;


    mAppData->xreDirectory = greDir;
  }

  if (aConfig.appData && aConfig.appDataPath) {
    mAppData->xreDirectory->Clone(getter_AddRefs(mAppData->directory));
    mAppData->directory->AppendNative(nsDependentCString(aConfig.appDataPath));
  }

  if (!mAppData->directory) {
    mAppData->directory = mAppData->xreDirectory;
  }


  mozilla::AutoIOInterposer ioInterposerGuard;
  ioInterposerGuard.Init();


  bool exit = false;
  int result = XRE_mainInit(&exit);
  if (result != 0 || exit) return result;

  auto cleanup = MakeScopeExit([&]() -> nsresult {
    if (mProfLD) {
      nsCOMPtr<nsIFile> crashFile = MOZ_TRY(GetIncompleteStartupFile(mProfLD));
      crashFile->Remove(false);
    }
    return NS_OK;
  });

  result = XRE_mainStartup(&exit);
  if (result != 0 || exit) return result;


  mScopedXPCOM = MakeUnique<ScopedXPCOMStartup>();

  rv = mScopedXPCOM->Initialize( false);
  if (rv == NS_ERROR_OMNIJAR_CORRUPT) {
    if (XRE_IsParentProcess()
#if defined(MOZ_BACKGROUNDTASKS)
        && !mozilla::BackgroundTasks::IsBackgroundTaskMode()
#endif
    ) {
      Output(
          true,
          "The installation seems to be corrupt.\nPlease check your hardware "
          "and disk setup\nand/or re-install.\n");
    }
    MOZ_CRASH("NS_ERROR_OMNIJAR_CORRUPT");
  }
  NS_ENSURE_SUCCESS(rv, 1);

  rv = XRE_mainRun();
  if (rv == NS_ERROR_OMNIJAR_OR_DIR_MISSING) {
    if (XRE_IsParentProcess()
#if defined(MOZ_BACKGROUNDTASKS)
        && !mozilla::BackgroundTasks::IsBackgroundTaskMode()
#endif
    ) {
      Output(true,
             "The installation seems to be incomplete.\nPlease check your "
             "hardware and disk setup\nand/or re-install.\n");
    }
    if (mozilla::IsPackagedBuild()) {
      MOZ_CRASH("NS_ERROR_OMNIJAR_CORRUPT");
    }
    MOZ_CRASH("NS_ERROR_OMNIJAR_OR_DIR_MISSING");
  }


  gAbsoluteArgv0Path.Truncate();

#if defined(MOZ_HAS_REMOTE)
  if (gRemoteService) {
    gRemoteService->ShutdownServer();
    gStartupLock = nullptr;
    gRemoteService = nullptr;
  }
#endif

  mScopedXPCOM = nullptr;

  mProfileLock->Unlock();
  gProfileLock = nullptr;

#if defined(MOZ_MINIMAL_BROWSER)
  if (mUsingEphemeralProfile) {
    (void)mProfD->Remove(true);
  }
#endif

  gLastAppVersion.Truncate();
  gLastAppBuildID.Truncate();

#if defined(MOZ_WIDGET_GTK)
#if defined(MOZ_WAYLAND)
  WaylandDisplayRelease();
  gWaylandProxy = nullptr;
#endif
#endif

  mozilla::AppShutdown::MaybeDoRestart();

  XRE_DeinitCommandLine();

  if (NS_FAILED(rv)) {
    return 1;
  }
  return mozilla::AppShutdown::GetExitCode();
}

int XRE_main(int argc, char* argv[], const BootstrapConfig& aConfig) {
  XREMain main;

  int result = main.XRE_main(argc, argv, aConfig);
#if defined(MOZ_BACKGROUNDTASKS)
  mozilla::BackgroundTasks::Shutdown();
#endif
  return result;
}

nsresult XRE_InitCommandLine(int aArgc, char* aArgv[]) {
  nsresult rv = NS_OK;


  char** canonArgs = new char*[aArgc];

  nsCOMPtr<nsIFile> binFile;
  rv = XRE_GetBinaryPath(getter_AddRefs(binFile));
  if (NS_FAILED(rv)) return NS_ERROR_FAILURE;

  nsAutoCString canonBinPath;
  rv = binFile->GetNativePath(canonBinPath);
  if (NS_FAILED(rv)) return NS_ERROR_FAILURE;

  canonArgs[0] = strdup(canonBinPath.get());

  for (int i = 1; i < aArgc; ++i) {
    if (aArgv[i]) {
      canonArgs[i] = strdup(aArgv[i]);
    }
  }

  NS_ASSERTION(!CommandLine::IsInitialized(), "Bad news!");
  CommandLine::Init(aArgc, canonArgs);

  for (int i = 0; i < aArgc; ++i) free(canonArgs[i]);
  delete[] canonArgs;


  return rv;
}

nsresult XRE_DeinitCommandLine() {
  nsresult rv = NS_OK;

  CommandLine::Terminate();

  return rv;
}

GeckoProcessType XRE_GetProcessType() { return GetGeckoProcessType(); }

const char* XRE_GetProcessTypeString() {
  return XRE_GeckoProcessTypeToString(XRE_GetProcessType());
}

GeckoChildID XRE_GetChildID() { return GetGeckoChildID(); }

bool XRE_IsE10sParentProcess() {
  return XRE_IsParentProcess() && BrowserTabsRemoteAutostart();
}

#define GECKO_PROCESS_TYPE(enum_value, enum_name, string_name, proc_typename, \
                           process_bin_type, procinfo_typename,               \
                           webidl_typename, allcaps_name)                     \
  bool XRE_Is##proc_typename##Process() {                                     \
    return XRE_GetProcessType() == GeckoProcessType_##enum_name;              \
  }
#include "mozilla/GeckoProcessTypes.h"
#undef GECKO_PROCESS_TYPE

bool XRE_UseNativeEventProcessing() {

  switch (XRE_GetProcessType()) {
    case GeckoProcessType_Content:
      return StaticPrefs::dom_ipc_useNativeEventProcessing_content();
    default:
      return true;
  }
}

namespace mozilla {

uint32_t GetMaxWebProcessCount() {
  if (Preferences::GetInt("dom.ipc.multiOptOut", 0) >=
      nsIXULRuntime::E10S_MULTI_EXPERIMENT) {
    return 1;
  }

  const char* optInPref = "dom.ipc.processCount";
  uint32_t optInPrefValue = Preferences::GetInt(optInPref, 1);
  return std::max(1u, optInPrefValue);
}

const char* PlatformBuildID() { return gToolkitBuildID; }

}  

void SetupErrorHandling(const char* progname) {

  InstallSignalHandlers(progname);

  setbuf(stdout, nullptr);
}

static bool gRunSelfAsContentProc = false;

void XRE_EnableSameExecutableForContentProc() {
  if (!PR_GetEnv("MOZ_SEPARATE_CHILD_PROCESS")) {
    gRunSelfAsContentProc = true;
  }
}

mozilla::BinPathType XRE_GetChildProcBinPathType(
    GeckoProcessType aProcessType) {
  MOZ_ASSERT(aProcessType != GeckoProcessType_Default);

  if (!gRunSelfAsContentProc) {
    return BinPathType::PluginContainer;
  }

  return BinPathType::Self;
}

extern "C" void install_rust_hooks();

struct InstallRustHooks {
  InstallRustHooks() { install_rust_hooks(); }
};

MOZ_RUNINIT InstallRustHooks sInstallRustHooks;
