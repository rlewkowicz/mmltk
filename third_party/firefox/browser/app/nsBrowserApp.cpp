/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsXULAppAPI.h"
#include "mozilla/XREAppData.h"
#include "XREChildData.h"
#include "ApplicationData.h"
#include "mozilla/Bootstrap.h"
#include "mozilla/ProcessType.h"
#include "BrowserDefines.h"
#if defined(XP_UNIX)
#  include <sys/resource.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include "nsCOMPtr.h"

#include "BinaryPath.h"

#include "nsXPCOMPrivate.h"  // for MAXPATHLEN and XPCOM_DLL
#include "mozilla/Sprintf.h"
#include "mozilla/StartupTimeline.h"


#if defined(MOZ_LINUX_32_SSE2_STARTUP_ERROR)
#  include <cpuid.h>

static bool IsSSE2Available() {
  unsigned int level = 1u;
  unsigned int eax, ebx, ecx, edx;
  unsigned int bits = (1u << 26);
  unsigned int max = __get_cpuid_max(0, nullptr);
  if (level > max) {
    return false;
  }
  __cpuid_count(level, 0, eax, ebx, ecx, edx);
  return (edx & bits) == bits;
}

static const char sSSE2Message[] =
    "This browser version requires a processor with the SSE2 instruction "
    "set extension.\nYou may be able to obtain a version that does not "
    "require SSE2 from your Linux distribution.\n";

__attribute__((constructor)) static void SSE2Check() {
  if (IsSSE2Available()) {
    return;
  }
  (void)write(STDERR_FILENO, sSSE2Message, std::size(sSSE2Message) - 1);
  _exit(255);
}
#endif

#  define MOZ_BROWSER_CAN_BE_CONTENTPROC

using namespace mozilla;

#define kDesktopFolder "browser"

#if defined(MOZ_BACKGROUNDTASKS)
static bool gIsBackgroundTask = false;
#endif

static MOZ_FORMAT_PRINTF(1, 2) void Output(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  vfprintf(stderr, fmt, ap);

  va_end(ap);
}

static bool IsFlag(const char* arg, const char* s) {
  if (*arg == '-') {
    if (*++arg == '-') ++arg;
    return !strcasecmp(arg, s);
  }


  return false;
}

#if defined(MOZ_BACKGROUNDTASKS)
static bool HasFlag(int argc, char* argv[], const char* s) {
  for (int i = 1; i < argc; i++) {
    if (IsFlag(argv[i], s)) {
      return true;
    }
  }
  return false;
}
#endif

constinit Bootstrap::UniquePtr gBootstrap;

static int do_main(int argc, char* argv[], char* envp[]) {
  const char* appDataFile = getenv("XUL_APP_FILE");
  if ((!appDataFile || !*appDataFile) && (argc > 1 && IsFlag(argv[1], "app"))) {
    if (argc == 2) {
      Output("Incorrect number of arguments passed to -app");
      return 255;
    }
    appDataFile = argv[2];

    char appEnv[MAXPATHLEN];
    SprintfLiteral(appEnv, "XUL_APP_FILE=%s", argv[2]);
    if (putenv(strdup(appEnv))) {
      Output("Couldn't set %s.\n", appEnv);
      return 255;
    }
    argv[2] = argv[0];
    argv += 2;
    argc -= 2;
  }

  BootstrapConfig config;

  if (appDataFile && *appDataFile) {
    config.appData = nullptr;
    config.appDataPath = appDataFile;
  } else {
    config.appData = kStaticAppData;
    config.appDataPath = kDesktopFolder;
  }



  EnsureBrowserCommandlineSafe(argc, argv);

  return gBootstrap->XRE_main(argc, argv, config);
}

static nsresult InitXPCOMGlue(LibLoadingStrategy aLibLoadingStrategy) {
  if (gBootstrap) {
    return NS_OK;
  }

  UniqueFreePtr<char> exePath = BinaryPath::Get();
  if (!exePath) {
    Output("Couldn't find the application directory.\n");
    return NS_ERROR_FAILURE;
  }

  auto bootstrapResult =
      mozilla::GetBootstrap(exePath.get(), aLibLoadingStrategy);
  if (bootstrapResult.isErr()) {
    Output("Couldn't load XPCOM.\n");
    return NS_ERROR_FAILURE;
  }

  gBootstrap = bootstrapResult.unwrap();

  gBootstrap->NS_LogInit();

  return NS_OK;
}

#if defined(HAS_DLL_BLOCKLIST)
uint32_t gBlocklistInitFlags = eDllBlocklistInitFlagDefault;
#endif

#if defined(XP_UNIX)
static void ReserveDefaultFileDescriptors() {
  int fd = open("/dev/null", O_RDONLY);
  for (int i = 0; i < 2; i++) {
    [[maybe_unused]] int r = dup(fd);
  }
}
#endif

static void ExpandFileDescriptorTable() {
  mozilla::UniqueFileHandle fdTableExpander(fcntl(0, F_DUPFD, 256));
}

int main(int argc, char* argv[], char* envp[]) {
#if defined(XP_UNIX)
  ReserveDefaultFileDescriptors();
#endif

#if defined(MOZ_BACKGROUNDTASKS)
  gIsBackgroundTask = HasFlag(argc, argv, "backgroundtask");
#endif

#if defined(MOZ_BROWSER_CAN_BE_CONTENTPROC)
  if (argc > 1 && IsFlag(argv[1], "contentproc")) {
    SetGeckoProcessType(argv[--argc]);
    SetGeckoChildID(argv[--argc]);

#if defined(MOZ_ENABLE_FORKSERVER)
    if (GetGeckoProcessType() == GeckoProcessType_ForkServer) {
      nsresult rv = InitXPCOMGlue(LibLoadingStrategy::NoReadAhead);
      if (NS_FAILED(rv)) {
        return 255;
      }

      if (gBootstrap->XRE_ForkServer(&argc, &argv)) {
        gBootstrap->NS_LogTerm();
        return 0;
      }
    }
#endif
  }
#endif

  ExpandFileDescriptorTable();

  mozilla::TimeStamp start = mozilla::TimeStamp::Now();


#if defined(MOZ_BROWSER_CAN_BE_CONTENTPROC)
  if (GetGeckoProcessType() != GeckoProcessType_Default) {

#if defined(HAS_DLL_BLOCKLIST)
    uint32_t initFlags =
        gBlocklistInitFlags | eDllBlocklistInitFlagIsChildProcess;
    SetDllBlocklistProcessTypeFlags(initFlags, GetGeckoProcessType());
    DllBlocklist_Initialize(initFlags);
#endif


    nsresult rv = InitXPCOMGlue(LibLoadingStrategy::NoReadAhead);
    if (NS_FAILED(rv)) {
      return 255;
    }

    XREChildData childData;


    rv = gBootstrap->XRE_InitChildProcess(argc, argv, &childData);

#if defined(DEBUG) && defined(HAS_DLL_BLOCKLIST)
    DllBlocklist_Shutdown();
#endif

    gBootstrap->NS_LogTerm();

    return NS_FAILED(rv) ? 1 : 0;
  }
#endif

#if defined(HAS_DLL_BLOCKLIST)
  DllBlocklist_Initialize(gBlocklistInitFlags);
#endif



  nsresult rv = InitXPCOMGlue(LibLoadingStrategy::ReadAhead);
  if (NS_FAILED(rv)) {
    return 255;
  }

  gBootstrap->XRE_StartupTimelineRecord(mozilla::StartupTimeline::START, start);

#if defined(MOZ_BROWSER_CAN_BE_CONTENTPROC)
  gBootstrap->XRE_EnableSameExecutableForContentProc();
#endif

  int result = do_main(argc, argv, envp);


  gBootstrap->NS_LogTerm();

#if defined(DEBUG) && defined(HAS_DLL_BLOCKLIST)
  DllBlocklist_Shutdown();
#endif

  gBootstrap.reset();

  return result;
}
