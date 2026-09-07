/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsAppRunner.h"
#include "nsXREDirProvider.h"
#  include "commonupdatedir.h"

#include "jsapi.h"
#include "xpcpublic.h"
#include "prprf.h"
#include "prenv.h"

#include "nsIAppStartup.h"
#include "nsIFile.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsISimpleEnumerator.h"
#include "nsIToolkitProfileService.h"
#include "nsIXULRuntime.h"
#include "commonupdatedir.h"

#include "nsAppDirectoryServiceDefs.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsXULAppAPI.h"
#include "nsCategoryManagerUtils.h"

#include "nsDependentString.h"
#include "nsCOMArray.h"
#include "nsArrayEnumerator.h"
#include "nsEnumeratorUtils.h"
#include "nsReadableUtils.h"

#include "SpecialSystemDirectory.h"

#include "mozilla/dom/ScriptSettings.h"

#include "mozilla/AppShutdown.h"
#include "mozilla/AutoRestore.h"
#if defined(MOZ_BACKGROUNDTASKS)
#  include "mozilla/BackgroundTasks.h"
#endif
#include "mozilla/CmdLineAndEnvUtils.h"
#include "mozilla/Components.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Services.h"
#include "mozilla/Omnijar.h"
#include "mozilla/Preferences.h"
#include "mozilla/Try.h"
#include "mozilla/Utf8.h"
#include "mozilla/XREAppData.h"
#include "nsPrintfCString.h"

#if defined(MOZ_THUNDERBIRD)
#  include "ScopedNSSTypes.h"
#  include "nsNSSComponent.h"
#endif

#include <stdlib.h>


#  define APP_REGISTRY_NAME "appreg"

#define PREF_OVERRIDE_DIRNAME "preferences"

nsXREDirProvider* gDirServiceProvider = nullptr;
nsIFile* gDataDirHomeLocal = nullptr;
nsIFile* gDataDirHome = nullptr;
constinit nsCOMPtr<nsIFile> gDataDirProfileLocal{};
constinit nsCOMPtr<nsIFile> gDataDirProfile{};

#if 0 || defined(XP_UNIX)
static const char* GetAppName() {
  if (gAppData) {
    return gAppData->name;
  }
  return nullptr;
}
#endif


nsXREDirProvider::nsXREDirProvider() { gDirServiceProvider = this; }

nsXREDirProvider::~nsXREDirProvider() {
  gDirServiceProvider = nullptr;
  gDataDirHomeLocal = nullptr;
  gDataDirHome = nullptr;
}

already_AddRefed<nsXREDirProvider> nsXREDirProvider::GetSingleton() {
  if (!gDirServiceProvider) {
    new nsXREDirProvider();  
  }
  return do_AddRef(gDirServiceProvider);
}

nsresult nsXREDirProvider::Initialize(nsIFile* aXULAppDir, nsIFile* aGREDir) {
  NS_ENSURE_ARG(aXULAppDir);
  NS_ENSURE_ARG(aGREDir);

  mXULAppDir = aXULAppDir;
  mGREDir = aGREDir;
  nsCOMPtr<nsIFile> binaryPath;
  nsresult rv = XRE_GetBinaryPath(getter_AddRefs(binaryPath));
  NS_ENSURE_SUCCESS(rv, rv);
  return binaryPath->GetParent(getter_AddRefs(mGREBinDir));
}

nsresult nsXREDirProvider::SetProfile(nsIFile* aDir, nsIFile* aLocalDir) {
  MOZ_ASSERT(aDir && aLocalDir, "We don't support no-profile apps!");
  MOZ_ASSERT(!mProfileDir && !mProfileLocalDir,
             "You may only set the profile directories once");

  nsresult rv = EnsureDirectoryExists(aDir);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = EnsureDirectoryExists(aLocalDir);
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoCString profilePath;
  rv = aDir->GetNativePath(profilePath);
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoCString localProfilePath;
  rv = aLocalDir->GetNativePath(localProfilePath);
  NS_ENSURE_SUCCESS(rv, rv);

  if (!mozilla::IsUtf8(profilePath) || !mozilla::IsUtf8(localProfilePath)) {
    PR_fprintf(
        PR_STDERR,
        "Error: The profile path is not valid UTF-8. Unable to continue.\n");
    return NS_ERROR_FAILURE;
  }


  mProfileDir = aDir;
  mProfileLocalDir = aLocalDir;
  return NS_OK;
}

NS_IMPL_QUERY_INTERFACE(nsXREDirProvider, nsIDirectoryServiceProvider,
                        nsIDirectoryServiceProvider2, nsIXREDirProvider,
                        nsIProfileStartup)

NS_IMETHODIMP_(MozExternalRefCountType)
nsXREDirProvider::AddRef() { return 1; }

NS_IMETHODIMP_(MozExternalRefCountType)
nsXREDirProvider::Release() { return 0; }

nsresult nsXREDirProvider::GetUserProfilesRootDir(nsIFile** aResult) {
  nsCOMPtr<nsIFile> file;
  nsresult rv = NS_OK;
  if (!file) {
    rv = GetUserDataDirectory(getter_AddRefs(file), false);
    NS_ENSURE_SUCCESS(rv, rv);
#if !defined(XP_UNIX) || 0
    rv = file->AppendNative("Profiles"_ns);
#endif
    nsresult tmp = EnsureDirectoryExists(file);
    if (NS_FAILED(tmp)) {
      rv = tmp;
    }

  }
  file.swap(*aResult);
  return rv;
}

nsresult nsXREDirProvider::GetUserProfilesLocalDir(nsIFile** aResult) {
  nsCOMPtr<nsIFile> file;
  nsresult rv = GetUserDataDirectory(getter_AddRefs(file), true);

  if (NS_SUCCEEDED(rv)) {
#if !defined(XP_UNIX) || 0
    rv = file->AppendNative("Profiles"_ns);
#endif
    nsresult tmp = EnsureDirectoryExists(file);
    if (NS_FAILED(tmp)) {
      rv = tmp;
    }
  }
  file.swap(*aResult);
  return NS_OK;
}

#if defined(MOZ_BACKGROUNDTASKS)
nsresult nsXREDirProvider::GetBackgroundTasksProfilesRootDir(
    nsIFile** aResult) {
  nsCOMPtr<nsIFile> file;
  nsresult rv = GetUserDataDirectory(getter_AddRefs(file), false);

  if (NS_SUCCEEDED(rv)) {
#if !defined(XP_UNIX) || 0
    rv = file->AppendNative("Background Tasks Profiles"_ns);
#endif
    nsresult tmp = EnsureDirectoryExists(file);
    if (NS_FAILED(tmp)) {
      rv = tmp;
    }
  }
  file.swap(*aResult);
  return rv;
}
#endif

#if defined(XP_UNIX) || 0
static nsresult GetSystemParentDirectory(nsIFile** aFile) {
  nsresult rv;
  nsCOMPtr<nsIFile> localDir;
  constexpr auto dirname =
#if defined(HAVE_USR_LIB64_DIR)
      "/usr/lib64/mozilla"_ns
#else
      "/usr/lib/mozilla"_ns
#endif
      ;
  rv = NS_NewNativeLocalFile(dirname, getter_AddRefs(localDir));

  if (NS_SUCCEEDED(rv)) {
    localDir.forget(aFile);
  }
  return rv;
}
#endif

NS_IMETHODIMP
nsXREDirProvider::GetFile(const char* aProperty, bool* aPersistent,
                          nsIFile** aFile) {
  *aPersistent = true;
  nsresult rv = NS_ERROR_FAILURE;

  nsCOMPtr<nsIFile> file;

  if (!strcmp(aProperty, NS_APP_USER_PROFILE_LOCAL_50_DIR) ||
      !strcmp(aProperty, NS_APP_PROFILE_LOCAL_DIR_STARTUP)) {
    if (mProfileLocalDir) {
      rv = mProfileLocalDir->Clone(getter_AddRefs(file));
    } else {
      NS_WARNING_ASSERTION(!XRE_IsParentProcess(),
                           "tried to get profile in parent too early");
      return NS_ERROR_FAILURE;
    }
  } else if (!strcmp(aProperty, NS_APP_USER_PROFILE_50_DIR) ||
             !strcmp(aProperty, NS_APP_PROFILE_DIR_STARTUP)) {
    rv = GetProfileStartupDir(getter_AddRefs(file));
    if (NS_FAILED(rv)) {
      return rv;
    }
  } else if (!strcmp(aProperty, NS_GRE_DIR)) {
    rv = mGREDir->Clone(getter_AddRefs(file));
  } else if (!strcmp(aProperty, NS_GRE_BIN_DIR)) {
    rv = mGREBinDir->Clone(getter_AddRefs(file));
  } else if (!strcmp(aProperty, NS_OS_CURRENT_PROCESS_DIR) ||
             !strcmp(aProperty, NS_APP_INSTALL_CLEANUP_DIR)) {
    rv = GetAppDir()->Clone(getter_AddRefs(file));
  } else if (!strcmp(aProperty, NS_APP_PREF_DEFAULTS_50_DIR)) {
    rv = mGREDir->Clone(getter_AddRefs(file));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = file->AppendNative("defaults"_ns);
    NS_ENSURE_SUCCESS(rv, rv);
    rv = file->AppendNative("pref"_ns);
  } else if (!strcmp(aProperty, NS_APP_APPLICATION_REGISTRY_DIR) ||
             !strcmp(aProperty, XRE_USER_APP_DATA_DIR)) {
    rv = GetUserAppDataDirectory(getter_AddRefs(file));
  }
#if defined(XP_UNIX) || 0
  else if (!strcmp(aProperty, XRE_SYS_NATIVE_MANIFESTS)) {
    rv = ::GetSystemParentDirectory(getter_AddRefs(file));
  } else if (!strcmp(aProperty, XRE_USER_NATIVE_MANIFESTS)) {
    rv = GetUserDataDirectoryHome(getter_AddRefs(file),  false,
                                   true);
    NS_ENSURE_SUCCESS(rv, rv);
    rv = file->AppendNative(".mozilla"_ns);
  }
#endif
  else if (!strcmp(aProperty, XRE_UPDATE_ROOT_DIR)) {
    rv = GetUpdateRootDir(getter_AddRefs(file));
  } else if (!strcmp(aProperty, XRE_OLD_UPDATE_ROOT_DIR)) {
    rv = GetUpdateRootDir(getter_AddRefs(file), true);
  } else if (!strcmp(aProperty, NS_APP_APPLICATION_REGISTRY_FILE)) {
    rv = GetUserAppDataDirectory(getter_AddRefs(file));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = file->AppendNative(nsLiteralCString(APP_REGISTRY_NAME));
  } else if (!strcmp(aProperty, NS_APP_USER_PROFILES_ROOT_DIR)) {
    rv = GetUserProfilesRootDir(getter_AddRefs(file));
  } else if (!strcmp(aProperty, NS_APP_USER_PROFILES_LOCAL_ROOT_DIR)) {
    rv = GetUserProfilesLocalDir(getter_AddRefs(file));
  } else if (!strcmp(aProperty, XRE_EXECUTABLE_FILE)) {
    rv = XRE_GetBinaryPath(getter_AddRefs(file));
  }
#if defined(XP_UNIX) || 0
  else if (!strcmp(aProperty, XRE_SYS_LOCAL_EXTENSION_PARENT_DIR)) {
#if defined(ENABLE_SYSTEM_EXTENSION_DIRS)
    rv = GetSystemExtensionsDirectory(getter_AddRefs(file));
#endif
  }
#endif
#if defined(XP_UNIX) && !0
  else if (!strcmp(aProperty, XRE_SYS_SHARE_EXTENSION_PARENT_DIR)) {
#if defined(ENABLE_SYSTEM_EXTENSION_DIRS)
    static const char* const sysLExtDir = "/usr/share/mozilla/extensions";
    rv = NS_NewNativeLocalFile(nsDependentCString(sysLExtDir),
                               getter_AddRefs(file));
#endif
  }
#endif
  else if (!strcmp(aProperty, XRE_USER_SYS_EXTENSION_DIR)) {
#if defined(ENABLE_SYSTEM_EXTENSION_DIRS)
    rv = GetSysUserExtensionsDirectory(getter_AddRefs(file));
#endif
  } else if (!strcmp(aProperty, XRE_USER_RUNTIME_DIR)) {
#if defined(XP_UNIX)
    nsPrintfCString path("/run/user/%d/%s/", getuid(), GetAppName());
    ToLowerCase(path);
    rv = NS_NewNativeLocalFile(path, getter_AddRefs(file));
#endif
  } else if (!strcmp(aProperty, XRE_APP_DISTRIBUTION_DIR)) {
    bool persistent = false;
    rv = GetFile(NS_GRE_DIR, &persistent, getter_AddRefs(file));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = file->AppendNative("distribution"_ns);
  } else if (!strcmp(aProperty, XRE_APP_FEATURES_DIR)) {
    rv = GetAppDir()->Clone(getter_AddRefs(file));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = file->AppendNative("features"_ns);
  } else if (!strcmp(aProperty, XRE_ADDON_APP_DIR)) {
    nsCOMPtr<nsIDirectoryServiceProvider> dirsvc(
        do_GetService("@mozilla.org/file/directory_service;1", &rv));
    NS_ENSURE_SUCCESS(rv, rv);
    bool unused;
    rv = dirsvc->GetFile("XCurProcD", &unused, getter_AddRefs(file));
  } else if (!strcmp(aProperty, NS_APP_USER_CHROME_DIR)) {
    rv = GetProfileStartupDir(getter_AddRefs(file));
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = file->AppendNative("chrome"_ns);
  } else if (!strcmp(aProperty, NS_APP_PREFS_50_DIR)) {
    rv = GetProfileDir(getter_AddRefs(file));
    if (NS_FAILED(rv)) {
      return rv;
    }
  } else if (!strcmp(aProperty, NS_APP_PREFS_50_FILE)) {
    rv = GetProfileDir(getter_AddRefs(file));
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = file->AppendNative("prefs.js"_ns);
  } else if (!strcmp(aProperty, NS_APP_PREFS_OVERRIDE_DIR)) {
    rv = GetProfileDir(getter_AddRefs(file));
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = file->AppendNative(nsLiteralCString(PREF_OVERRIDE_DIRNAME));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = EnsureDirectoryExists(file);
  } else {
    return NS_ERROR_FAILURE;
  }

  NS_ENSURE_SUCCESS(rv, rv);
  NS_ENSURE_TRUE(file, NS_ERROR_FAILURE);

  file.forget(aFile);
  return NS_OK;
}

static void LoadDirIntoArray(nsIFile* dir, const char* const* aAppendList,
                             nsCOMArray<nsIFile>& aDirectories) {
  if (!dir) return;

  nsCOMPtr<nsIFile> subdir;
  dir->Clone(getter_AddRefs(subdir));
  if (!subdir) return;

  for (const char* const* a = aAppendList; *a; ++a) {
    subdir->AppendNative(nsDependentCString(*a));
  }

  bool exists;
  if (NS_SUCCEEDED(subdir->Exists(&exists)) && exists) {
    aDirectories.AppendObject(subdir);
  }
}

static const char* const kAppendPrefDir[] = {"defaults", "preferences",
                                             nullptr};
#if defined(MOZ_BACKGROUNDTASKS)
static const char* const kAppendBackgroundTasksPrefDir[] = {
    "defaults", "backgroundtasks", nullptr};
#endif

NS_IMETHODIMP
nsXREDirProvider::GetFiles(const char* aProperty,
                           nsISimpleEnumerator** aResult) {
  nsresult rv = NS_ERROR_FAILURE;
  *aResult = nullptr;

  if (!strcmp(aProperty, NS_APP_PREFS_DEFAULTS_DIR_LIST)) {
    nsCOMArray<nsIFile> directories;

    LoadDirIntoArray(mXULAppDir, kAppendPrefDir, directories);
#if defined(MOZ_BACKGROUNDTASKS)
    if (mozilla::BackgroundTasks::IsBackgroundTaskMode()) {
      LoadDirIntoArray(mGREDir, kAppendBackgroundTasksPrefDir, directories);
      LoadDirIntoArray(mXULAppDir, kAppendBackgroundTasksPrefDir, directories);
    }
#endif

    rv = NS_NewArrayEnumerator(aResult, directories, NS_GET_IID(nsIFile));
  } else if (!strcmp(aProperty, NS_APP_CHROME_DIR_LIST)) {

    static const char* const kAppendChromeDir[] = {"chrome", nullptr};
    nsCOMArray<nsIFile> directories;
    LoadDirIntoArray(mXULAppDir, kAppendChromeDir, directories);

    rv = NS_NewArrayEnumerator(aResult, directories, NS_GET_IID(nsIFile));
  }
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_SUCCESS_AGGREGATE_RESULT;
}

NS_IMETHODIMP
nsXREDirProvider::GetDirectory(nsIFile** aResult) {
  NS_ENSURE_TRUE(mProfileDir, NS_ERROR_NOT_INITIALIZED);
  return mProfileDir->Clone(aResult);
}

void nsXREDirProvider::InitializeUserPrefs() {
  if (!mPrefsInitialized) {
    mozilla::Preferences::InitializeUserPrefs();
  }
}

void nsXREDirProvider::FinishInitializingUserPrefs() {
  if (!mPrefsInitialized) {
    mozilla::Preferences::FinishInitializingUserPrefs();
    mPrefsInitialized = true;
  }
}

NS_IMETHODIMP
nsXREDirProvider::DoStartup() {
  nsresult rv;

  if (!mAppStarted) {
    nsCOMPtr<nsIObserverService> obsSvc =
        mozilla::services::GetObserverService();
    if (!obsSvc) return NS_ERROR_FAILURE;

    mAppStarted = true;

    MOZ_ASSERT(mPrefsInitialized);

    bool safeModeNecessary = false;
    nsCOMPtr<nsIAppStartup> appStartup(
        mozilla::components::AppStartup::Service());
    if (appStartup) {
      rv = appStartup->TrackStartupCrashBegin(&safeModeNecessary);
      if (NS_FAILED(rv) && rv != NS_ERROR_NOT_AVAILABLE)
        NS_WARNING("Error while beginning startup crash tracking");

      if (!gSafeMode && safeModeNecessary) {
        appStartup->RestartInSafeMode(nsIAppStartup::eForceQuit);
        return NS_OK;
      }
    }

    static const char16_t kStartup[] = {'s', 't', 'a', 'r',
                                        't', 'u', 'p', '\0'};
    obsSvc->NotifyObservers(nullptr, "profile-do-change", kStartup);

    if (XRE_IsParentProcess()) {
      nsCOMPtr<nsIObserver> policies(
          do_GetService("@mozilla.org/enterprisepolicies;1"));
      if (policies) {
        policies->Observe(nullptr, "policies-startup", nullptr);
      }
    }

#if defined(MOZ_THUNDERBIRD)
    bool bgtaskMode = false;
#if defined(MOZ_BACKGROUNDTASKS)
    bgtaskMode = mozilla::BackgroundTasks::IsBackgroundTaskMode();
#endif
    if (!bgtaskMode &&
        mozilla::Preferences::GetBool(
            "security.prompt_for_master_password_on_startup", false)) {
      nsCOMPtr<nsINSSComponent> nssComponent(
          do_GetService(NS_NSSCOMPONENT_CID));
      mozilla::UniquePK11SlotInfo slot(PK11_GetInternalKeySlot());
      if (slot) {
        (void)PK11_Authenticate(slot.get(), true, nullptr);
      }
    }
#endif

    bool initExtensionManager =
#if defined(MOZ_BACKGROUNDTASKS)
        !mozilla::BackgroundTasks::IsBackgroundTaskMode();
#else
        true;
#endif
    if (initExtensionManager) {
      nsCOMPtr<nsIObserver> em =
          do_GetService("@mozilla.org/addons/integration;1");
      if (em) {
        em->Observe(nullptr, "addons-startup", nullptr);
      } else {
        NS_WARNING("Failed to create Addons Manager.");
      }
    }

    obsSvc->NotifyObservers(nullptr, "profile-after-change", kStartup);

    (void)NS_CreateServicesFromCategory("profile-after-change", nullptr,
                                        "profile-after-change");

    if (gSafeMode && safeModeNecessary) {
      static const char16_t kCrashed[] = {'c', 'r', 'a', 's',
                                          'h', 'e', 'd', '\0'};
      obsSvc->NotifyObservers(nullptr, "safemode-forced", kCrashed);
    }

    obsSvc->NotifyObservers(nullptr, "profile-initial-state", nullptr);
  }
  return NS_OK;
}

void nsXREDirProvider::DoShutdown() {

  if (mAppStarted) {
    mozilla::AppShutdown::AdvanceShutdownPhase(
        mozilla::ShutdownPhase::AppShutdownNetTeardown, nullptr);
    mozilla::AppShutdown::AdvanceShutdownPhase(
        mozilla::ShutdownPhase::AppShutdownTeardown, nullptr);

#if defined(DEBUG)
    if (JSContext* cx = mozilla::dom::danger::GetJSContext()) {
      JS_GC(cx);
    }
#endif

    mozilla::AppShutdown::AdvanceShutdownPhase(
        mozilla::ShutdownPhase::AppShutdown, nullptr);
    mozilla::AppShutdown::AdvanceShutdownPhase(
        mozilla::ShutdownPhase::AppShutdownQM, nullptr);
    mozilla::AppShutdown::AdvanceShutdownPhase(
        mozilla::ShutdownPhase::AppShutdownTelemetry, nullptr);
    mAppStarted = false;
  }

  gDataDirProfileLocal = nullptr;
  gDataDirProfile = nullptr;
}


static nsresult HashInstallPath(nsAString& aInstallPath, nsAString& aPathHash) {
  mozilla::UniquePtr<NS_tchar[]> hash;
  bool success = ::GetInstallHash(PromiseFlatString(aInstallPath).get(), hash);
  if (!success) {
    return NS_ERROR_FAILURE;
  }

  aPathHash.AssignASCII(hash.get());
  return NS_OK;
}

nsresult nsXREDirProvider::GetInstallHash(nsAString& aPathHash) {
  nsAutoString stringToHash;

  {
    nsCOMPtr<nsIFile> installDir;
    nsCOMPtr<nsIFile> appFile;
    bool per = false;
    nsresult rv = GetFile(XRE_EXECUTABLE_FILE, &per, getter_AddRefs(appFile));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = appFile->GetParent(getter_AddRefs(installDir));
    NS_ENSURE_SUCCESS(rv, rv);


    rv = installDir->GetPath(stringToHash);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (stringToHash.IsEmpty()) {
    return NS_ERROR_FAILURE;
  }

  return HashInstallPath(stringToHash, aPathHash);
}

nsresult nsXREDirProvider::GetLegacyInstallHash(nsAString& aPathHash) {
  nsCOMPtr<nsIFile> installDir;
  nsCOMPtr<nsIFile> appFile;
  bool per = false;
  nsresult rv = GetFile(XRE_EXECUTABLE_FILE, &per, getter_AddRefs(appFile));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = appFile->GetParent(getter_AddRefs(installDir));
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoString installPath;
  rv = installDir->GetPath(installPath);
  NS_ENSURE_SUCCESS(rv, rv);

  return HashInstallPath(installPath, aPathHash);
}

nsresult nsXREDirProvider::GetUpdateRootDir(nsIFile** aResult,
                                            bool aGetOldLocation) {
  if (aGetOldLocation) {
    return NS_ERROR_NOT_IMPLEMENTED;
  }
  nsCOMPtr<nsIFile> updRoot;
  nsCOMPtr<nsIFile> appFile;
  bool per = false;
  nsresult rv = GetFile(XRE_EXECUTABLE_FILE, &per, getter_AddRefs(appFile));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = appFile->GetParent(getter_AddRefs(updRoot));
  NS_ENSURE_SUCCESS(rv, rv);

  updRoot.forget(aResult);
  return NS_OK;
}

nsresult nsXREDirProvider::GetProfileStartupDir(nsIFile** aResult) {
  if (mProfileDir) {
    return mProfileDir->Clone(aResult);
  }

  NS_WARNING_ASSERTION(!XRE_IsParentProcess(),
                       "tried to get profile in parent too early");
  return NS_ERROR_FAILURE;
}

nsresult nsXREDirProvider::GetProfileDir(nsIFile** aResult) {
  if (!mProfileDir) {
    nsresult rv = NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR,
                                         getter_AddRefs(mProfileDir));
    if (NS_FAILED(rv)) {
      MOZ_ASSERT(!mProfileDir,
                 "Directory provider failed but returned a value");
      mProfileDir = nullptr;
    }
  }
  return GetProfileStartupDir(aResult);
}

NS_IMETHODIMP
nsXREDirProvider::SetUserDataDirectory(nsIFile* aFile, bool aLocal) {
  if (aLocal) {
    NS_IF_RELEASE(gDataDirHomeLocal);
    NS_IF_ADDREF(gDataDirHomeLocal = aFile);
  } else {
    NS_IF_RELEASE(gDataDirHome);
    NS_IF_ADDREF(gDataDirHome = aFile);
  }

  return NS_OK;
}

nsresult nsXREDirProvider::SetUserDataProfileDirectory(nsCOMPtr<nsIFile>& aFile,
                                                       bool aLocal) {
  if (aLocal) {
    gDataDirProfileLocal = aFile;
  } else {
    gDataDirProfile = aFile;
  }

  return NS_OK;
}

nsresult nsXREDirProvider::ClearUserDataProfileDirectoryFromGTest(
    nsIFile** aLocal, nsIFile** aGlobal) {
  if (gDataDirProfileLocal) {
    gDataDirProfileLocal->Clone(aLocal);
    gDataDirProfileLocal = nullptr;
  }

  if (gDataDirProfile) {
    gDataDirProfile->Clone(aGlobal);
    gDataDirProfile = nullptr;
  }

  return NS_OK;
}

nsresult nsXREDirProvider::RestoreUserDataProfileDirectoryFromGTest(
    nsCOMPtr<nsIFile>& aLocal, nsCOMPtr<nsIFile>& aGlobal) {
  gDataDirProfileLocal = aLocal;
  gDataDirProfile = aGlobal;

  return NS_OK;
}

nsresult nsXREDirProvider::GetUserDataDirectoryHome(nsIFile** aFile,
                                                    bool aLocal,
                                                    bool aForceLegacy) {
  nsCOMPtr<nsIFile> localDir;

  if (aLocal && gDataDirHomeLocal) {
    return gDataDirHomeLocal->Clone(aFile);
  }
  if (!aLocal && gDataDirHome) {
    return gDataDirHome->Clone(aFile);
  }

#if defined(XP_UNIX)
  const char* homeDir = PR_GetEnv("HOME");
  if (!homeDir || !*homeDir) return NS_ERROR_FAILURE;

  if (aLocal) {
    MOZ_TRY(nsXREDirProvider::GetLegacyOrXDGCachePath(
        homeDir, getter_AddRefs(localDir)));
  } else {
    MOZ_TRY(nsXREDirProvider::GetLegacyOrXDGHomePath(
        homeDir, getter_AddRefs(localDir), aForceLegacy));
  }
#else
#  error "Don't know how to get product dir on your platform"
#endif

  localDir.forget(aFile);
  return NS_OK;
}

nsresult nsXREDirProvider::GetSysUserExtensionsDirectory(nsIFile** aFile) {
  nsCOMPtr<nsIFile> localDir;
  nsresult rv = GetUserDataDirectoryHome(
      getter_AddRefs(localDir),  false,  true);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = AppendSysUserExtensionPath(localDir);
  NS_ENSURE_SUCCESS(rv, rv);


  localDir.forget(aFile);
  return NS_OK;
}

#if defined(XP_UNIX) || 0
nsresult nsXREDirProvider::GetSystemExtensionsDirectory(nsIFile** aFile) {
  nsresult rv;
  nsCOMPtr<nsIFile> localDir;

  rv = GetSystemParentDirectory(getter_AddRefs(localDir));
  if (NS_SUCCEEDED(rv)) {
    constexpr auto sExtensions =
        "extensions"_ns
        ;

    rv = localDir->AppendNative(sExtensions);
    if (NS_SUCCEEDED(rv)) {
      localDir.forget(aFile);
    }
  }
  return rv;
}
#endif

nsresult nsXREDirProvider::GetUserDataDirectory(nsIFile** aFile, bool aLocal) {
  nsCOMPtr<nsIFile> localDir;
  nsCOMPtr<nsIFile> customDir = mozilla::GetFileFromEnv("MOZ_APP_DATA");
  nsCOMPtr<nsIFile> customLocalDir =
      mozilla::GetFileFromEnv("MOZ_LOCAL_APP_DATA");

  if (aLocal && gDataDirProfileLocal) {
    return gDataDirProfileLocal->Clone(aFile);
  }
  if (!aLocal && gDataDirProfile) {
    return gDataDirProfile->Clone(aFile);
  }

  nsresult rv;
  if (aLocal && customLocalDir) {
    localDir = customLocalDir;
  } else if (!aLocal && customDir) {
    localDir = customDir;
  } else {
    rv = GetUserDataDirectoryHome(getter_AddRefs(localDir), aLocal);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = AppendProfilePath(localDir, aLocal);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  rv = EnsureDirectoryExists(localDir);
  NS_ENSURE_SUCCESS(rv, rv);

  nsXREDirProvider::SetUserDataProfileDirectory(localDir, aLocal);

  localDir.forget(aFile);
  return NS_OK;
}

nsresult nsXREDirProvider::EnsureDirectoryExists(nsIFile* aDirectory) {
  nsresult rv = aDirectory->Create(nsIFile::DIRECTORY_TYPE, 0700);

  if (rv == NS_ERROR_FILE_ALREADY_EXISTS) {
    rv = NS_OK;
  }
  return rv;
}

nsresult nsXREDirProvider::AppendSysUserExtensionPath(nsIFile* aFile) {
  NS_ASSERTION(aFile, "Null pointer!");

  nsresult rv;

#if defined(XP_UNIX)

  static const char* const sXR = ".mozilla";
  rv = aFile->AppendNative(nsDependentCString(sXR));
  NS_ENSURE_SUCCESS(rv, rv);

  static const char* const sExtensions = "extensions";
  rv = aFile->AppendNative(nsDependentCString(sExtensions));
  NS_ENSURE_SUCCESS(rv, rv);

#else
#  error "Don't know how to get XRE user extension path on your platform"
#endif
  return NS_OK;
}

#if defined(MOZ_WIDGET_GTK)
bool nsXREDirProvider::IsForceLegacyHome() {
#if !defined(MOZ_LEGACY_HOME)
  const char* legacyhomedir = PR_GetEnv("MOZ_LEGACY_HOME");
  return legacyhomedir && legacyhomedir[0] == '1';
#else
  return true;
#endif
}

nsresult nsXREDirProvider::AppendFromAppData(nsIFile* aFile, bool aIsDotted) {
  if (gAppData->profile) {
    nsAutoCString profile;
#if defined(MOZ_THUNDERBIRD)
    if (gAppData->profile[0] != '.') {
      profile.Assign('.');
    }
#endif
    profile.Append(gAppData->profile);
    MOZ_TRY(aFile->AppendRelativeNativePath(profile));
  } else {
    nsAutoCString vendor;
    nsAutoCString appName;
    vendor = gAppData->vendor;
    appName = gAppData->name;
    ToLowerCase(vendor);
    ToLowerCase(appName);

    MOZ_TRY(aFile->AppendRelativeNativePath(aIsDotted ? ("."_ns + vendor)
                                                      : vendor));
    MOZ_TRY(aFile->AppendRelativeNativePath(appName));
  }

  return NS_OK;
}

bool nsXREDirProvider::LegacyHomeExists(nsIFile** aFile) {
  bool exists;
  nsDependentCString homeDir(PR_GetEnv("HOME"));
  nsCOMPtr<nsIFile> localDir;
  nsCOMPtr<nsIFile> parentDir;

  nsresult rv = NS_NewNativeLocalFile(homeDir, getter_AddRefs(localDir));
  NS_ENSURE_SUCCESS(rv, false);

  rv = localDir->Clone(getter_AddRefs(parentDir));
  NS_ENSURE_SUCCESS(rv, false);

  rv = AppendFromAppData(localDir, true);
  NS_ENSURE_SUCCESS(rv, false);

  rv = localDir->Exists(&exists);
  NS_ENSURE_SUCCESS(rv, false);

  if (!exists) {
    nsCOMPtr<nsIFile> userDir;
    rv = parentDir->Clone(getter_AddRefs(userDir));
    NS_ENSURE_SUCCESS(rv, false);

    nsAutoCString mozUserDir;
    mozUserDir = nsLiteralCString(MOZ_USER_DIR);

    rv = userDir->AppendRelativeNativePath(mozUserDir);
    NS_ENSURE_SUCCESS(rv, false);

    rv = userDir->Exists(&exists);
    NS_ENSURE_SUCCESS(rv, false);
  }

  if (aFile) {
    parentDir.forget(aFile);
  }

  return exists;
}

nsresult nsXREDirProvider::GetLegacyOrXDGEnvValue(const char* aHomeDir,
                                                  const char* aEnvName,
                                                  nsCString aSubdir,
                                                  nsIFile** aFile,
                                                  bool* aWasFromEnv) {
  nsCOMPtr<nsIFile> localDir;
  nsresult rv = NS_OK;

  const char* envValue = PR_GetEnv(aEnvName);
  if (envValue && *envValue) {
    rv = NS_NewNativeLocalFile(nsDependentCString(envValue),
                               getter_AddRefs(localDir));
    if (aWasFromEnv) {
      *aWasFromEnv = true;
    }
  }

  if (NS_FAILED(rv) || !envValue || !*envValue) {
    MOZ_TRY(NS_NewNativeLocalFile(nsDependentCString(aHomeDir),
                                  getter_AddRefs(localDir)));
    MOZ_TRY(localDir->AppendNative(aSubdir));
    if (aWasFromEnv) {
      *aWasFromEnv = false;
    }
  }

  localDir.forget(aFile);
  return NS_OK;
}

nsresult nsXREDirProvider::GetLegacyOrXDGCachePath(const char* aHomeDir,
                                                   nsIFile** aFile) {
  return GetLegacyOrXDGEnvValue(aHomeDir, "XDG_CACHE_HOME", ".cache"_ns, aFile,
                                nullptr);
}

nsresult nsXREDirProvider::GetLegacyOrXDGConfigHome(const char* aHomeDir,
                                                    nsIFile** aFile) {
  return GetLegacyOrXDGEnvValue(aHomeDir, "XDG_CONFIG_HOME", ".config"_ns,
                                aFile, nullptr);
}

nsresult nsXREDirProvider::GetLegacyOrXDGHomePath(const char* aHomeDir,
                                                  nsIFile** aFile,
                                                  bool aForceLegacy) {
  nsCOMPtr<nsIFile> parentDir;
  nsDependentCString homeDir(aHomeDir);

  bool exists = LegacyHomeExists(getter_AddRefs(parentDir));
  if (exists || IsForceLegacyHome() || aForceLegacy) {
    parentDir.forget(aFile);
    return NS_OK;
  }

  nsCOMPtr<nsIFile> localDir;

  nsAutoCString mozUserDir;
  mozUserDir = nsLiteralCString(MOZ_USER_DIR);
  if (mozUserDir.get()[0] == '.') {
    MOZ_TRY(NS_NewNativeLocalFile(nsDependentCString(aHomeDir),
                                  getter_AddRefs(localDir)));
    MOZ_TRY(localDir->AppendRelativeNativePath(mozUserDir));
  } else {
    if (gAppData->profile
#if defined(MOZ_THUNDERBIRD)
        && strcmp(gAppData->profile, "thunderbird") != 0 &&
        strcmp(gAppData->profile, "Thunderbird") != 0
#endif
    ) {
      MOZ_TRY(NS_NewNativeLocalFile(nsDependentCString(aHomeDir),
                                    getter_AddRefs(localDir)));
    } else {
      MOZ_TRY(GetLegacyOrXDGConfigHome(aHomeDir, getter_AddRefs(localDir)));
      MOZ_TRY(localDir->Clone(getter_AddRefs(parentDir)));
    }

    MOZ_TRY(AppendFromAppData(localDir, false));
  }

  if (aFile) {
    parentDir.forget(aFile);
  }

  MOZ_TRY(EnsureDirectoryExists(localDir));

  return NS_OK;
}
#endif

nsresult nsXREDirProvider::AppendProfilePath(nsIFile* aFile, bool aLocal) {
  NS_ASSERTION(aFile, "Null pointer!");

  if (!gAppData) {
    return NS_OK;
  }


  nsAutoCString profile;
  nsAutoCString appName;
  nsAutoCString vendor;
  if (gAppData->profile) {
    profile = gAppData->profile;
  } else {
    appName = gAppData->name;
    vendor = gAppData->vendor;
  }

  nsresult rv = NS_OK;

#if defined(XP_UNIX)
  nsAutoCString folder;
  if (!aLocal
#if defined(MOZ_WIDGET_GTK)
      && (IsForceLegacyHome() || LegacyHomeExists(nullptr))
#endif
  ) {
    folder.Assign('.');
  }

  if (!profile.IsEmpty()) {
    const char* profileStart = profile.get();
    while (*profileStart == '/' || *profileStart == '\\') profileStart++;

    if (*profileStart == '.' && !aLocal) profileStart++;

    folder.Append(profileStart);
    ToLowerCase(folder);

    rv = AppendProfileString(aFile, folder.get());
  } else {
    if (!vendor.IsEmpty()) {
      folder.Append(vendor);
      ToLowerCase(folder);

      rv = aFile->AppendNative(folder);
      NS_ENSURE_SUCCESS(rv, rv);

      folder.Truncate();
    }

    if (!appName.IsEmpty()) {
      folder.Append(appName);
      ToLowerCase(folder);

      rv = aFile->AppendNative(folder);
    }
  }
  NS_ENSURE_SUCCESS(rv, rv);

#else
#  error "Don't know how to get profile path on your platform"
#endif
  return NS_OK;
}

nsresult nsXREDirProvider::AppendProfileString(nsIFile* aFile,
                                               const char* aPath) {
  NS_ASSERTION(aFile, "Null file!");
  NS_ASSERTION(aPath, "Null path!");

  nsAutoCString pathDup(aPath);

  char* path = pathDup.BeginWriting();

  nsresult rv;
  char* subdir;
  while ((subdir = NS_strtok("/\\", &path))) {
    rv = aFile->AppendNative(nsDependentCString(subdir));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}
