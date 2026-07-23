#filter substitution
#include @TOPOBJDIR@/source-repo.h
#include @TOPOBJDIR@/buildid.h
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

export var AppConstants = Object.freeze({
  NIGHTLY_BUILD: @NIGHTLY_BUILD_BOOL@,

  ENABLE_EXPLICIT_RESOURCE_MANAGEMENT: @ENABLE_EXPLICIT_RESOURCE_MANAGEMENT_BOOL@,

  RELEASE_OR_BETA: @RELEASE_OR_BETA_BOOL@,

  EARLY_BETA_OR_EARLIER: @EARLY_BETA_OR_EARLIER_BOOL@,

  IS_ESR: @MOZ_ESR_BOOL@,

  ACCESSIBILITY: @ACCESSIBILITY_BOOL@,

  MOZILLA_OFFICIAL: @MOZILLA_OFFICIAL_BOOL@,

  MOZ_OFFICIAL_BRANDING: @MOZ_OFFICIAL_BRANDING_BOOL@,

  BUILT_BY_MOZILLA:
#if defined(BUILT_BY_MOZILLA)
  true,
#else
  false,
#endif

  MOZ_DEV_EDITION: @MOZ_DEV_EDITION_BOOL@,

  MOZ_UPDATER: @MOZ_UPDATER_BOOL@,

  MOZ_WIDGET_GTK:
#if defined(MOZ_WIDGET_GTK)
  true,
#else
  false,
#endif

  XP_UNIX:
#if defined(XP_UNIX)
  true,
#else
  false,
#endif

  platform:
#if defined(MOZ_WIDGET_GTK)
  "linux",
#elif XP_LINUX
  "linux",
#else
  "other",
#endif

  unixstyle:
    "linux",

  isPlatformAndVersionAtLeast(platform, version) {
    let platformVersion = Services.sysinfo.getProperty("version");
    return platform == this.platform &&
           Services.vc.compare(platformVersion, version) >= 0;
  },

  isPlatformAndVersionAtMost(platform, version) {
    let platformVersion = Services.sysinfo.getProperty("version");
    return platform == this.platform &&
           Services.vc.compare(platformVersion, version) <= 0;
  },

  MOZ_MINIMAL_BROWSER: @MOZ_MINIMAL_BROWSER_BOOL@,

  MOZ_NORMANDY: @MOZ_NORMANDY_BOOL@,

  MOZ_MAINTENANCE_SERVICE: @MOZ_MAINTENANCE_SERVICE_BOOL@,

  MOZ_BACKGROUNDTASKS: @MOZ_BACKGROUNDTASKS_BOOL@,

  MOZ_UPDATE_AGENT: @MOZ_UPDATE_AGENT_BOOL@,

  DEBUG: @MOZ_DEBUG_BOOL@,

  ASAN: @MOZ_ASAN_BOOL@,

  TSAN: @MOZ_TSAN_BOOL@,

  MOZ_SYSTEM_NSS: @MOZ_SYSTEM_NSS_BOOL@,

  MOZ_PLACES: @MOZ_PLACES_BOOL@,

  MOZ_GECKOVIEW_HISTORY: @MOZ_GECKOVIEW_HISTORY_BOOL@,

  DLL_PREFIX: "@DLL_PREFIX@",
  DLL_SUFFIX: "@DLL_SUFFIX@",

  MOZ_APP_NAME: "@MOZ_APP_NAME@",
  MOZ_APP_BASENAME: "@MOZ_APP_BASENAME@",
  MOZ_APP_DISPLAYNAME_DO_NOT_USE: "@MOZ_APP_DISPLAYNAME@",
  MOZ_APP_VERSION: "@MOZ_APP_VERSION@",
  MOZ_APP_VERSION_DISPLAY: "@MOZ_APP_VERSION_DISPLAY@",
  MOZ_BUILDID: "@MOZ_BUILDID@",
  MOZ_BUILD_APP: "@MOZ_BUILD_APP@",
  MOZ_MACBUNDLE_ID: "@MOZ_MACBUNDLE_ID@",
  MOZ_MACBUNDLE_NAME: "@MOZ_MACBUNDLE_NAME@",
  MOZ_UPDATE_CHANNEL: "@MOZ_UPDATE_CHANNEL@",
  MOZ_WIDGET_TOOLKIT: "@MOZ_WIDGET_TOOLKIT@",

  DEBUG_JS_MODULES: "@DEBUG_JS_MODULES@",

  MOZ_BING_API_CLIENTID: "@MOZ_BING_API_CLIENTID@",
  MOZ_BING_API_KEY: "@MOZ_BING_API_KEY@",
  MOZ_GOOGLE_LOCATION_SERVICE_API_KEY: "@MOZ_GOOGLE_LOCATION_SERVICE_API_KEY@",
  MOZ_MOZILLA_API_KEY: "@MOZ_MOZILLA_API_KEY@",

  BROWSER_CHROME_URL: "@BROWSER_CHROME_URL@",

  OMNIJAR_NAME: "@OMNIJAR_NAME@",

#if !defined(MOZ_SOURCE_URL)
#define MOZ_SOURCE_URL
#endif
  SOURCE_REVISION_URL: "@MOZ_SOURCE_URL@",

  HAVE_USR_LIB64_DIR:
#if defined(HAVE_USR_LIB64_DIR)
    true,
#else
    false,
#endif

  HAVE_SHELL_SERVICE: @HAVE_SHELL_SERVICE_BOOL@,

  MOZ_CODE_COVERAGE: @MOZ_CODE_COVERAGE_BOOL@,

  REMOTE_SETTINGS_SERVER_URLS:
#if defined(MOZ_THUNDERBIRD)
    [ "https://thunderbird-settings.thunderbird.net/v1" ],
#else
    [ "https://firefox.settings.services.mozilla.com/v1", "https://firefox.settings.services.mozilla.com/v2" ],
#endif

  REMOTE_SETTINGS_VERIFY_SIGNATURE:
#if defined(MOZ_THUNDERBIRD)
    false,
#else
    true,
#endif

  REMOTE_SETTINGS_DEFAULT_BUCKET:
#if defined(MOZ_THUNDERBIRD)
    "thunderbird",
#else
    "main",
#endif

  MOZ_JXL: @MOZ_JXL_BOOL@,

#if defined(MOZ_THUNDERBIRD) || defined(MOZ_SUITE)
  MOZ_CAN_FOLLOW_SYSTEM_TIME:
#if MOZ_WIDGET_GTK
  #if defined(MOZ_ENABLE_DBUS)
    true,
  #else
    false,
  #endif
#else
    false,
#endif
#endif

  MOZ_SYSTEM_POLICIES: @MOZ_SYSTEM_POLICIES_BOOL@,

  MOZ_SELECTABLE_PROFILES: @MOZ_SELECTABLE_PROFILES_BOOL@,

  SQLITE_LIBRARY_FILENAME:
#if defined(MOZ_FOLD_LIBS)
  "@DLL_PREFIX@nss3@DLL_SUFFIX@",
#else
  "@DLL_PREFIX@mozsqlite3@DLL_SUFFIX@",
#endif

  MOZ_GECKOVIEW:
#if defined(MOZ_GECKOVIEW)
    true,
#else
    false,
#endif


  USE_LIBZ_RS: @USE_LIBZ_RS_BOOL@,
});
