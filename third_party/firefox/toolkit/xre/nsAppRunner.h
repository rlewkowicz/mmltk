/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(nsAppRunner_h_)
#define nsAppRunner_h_

#  include <limits.h>

#if !defined(MAXPATHLEN)
#if defined(PATH_MAX)
#    define MAXPATHLEN PATH_MAX
#elif defined(_MAX_PATH)
#    define MAXPATHLEN _MAX_PATH
#elif defined(CCHMAXPATH)
#    define MAXPATHLEN CCHMAXPATH
#else
#    define MAXPATHLEN 1024
#endif
#endif

#include "nsCOMPtr.h"
#include "nsStringFwd.h"
#include "nsXULAppAPI.h"
#if defined(MOZ_HAS_REMOTE)
#  include "nsIRemoteService.h"
#endif

class nsINativeAppSupport;
class nsXREDirProvider;
class nsIToolkitProfileService;
class nsIFile;
class nsIProfileLock;
class nsIProfileUnlocker;
class nsIFactory;

extern nsXREDirProvider* gDirServiceProvider;

extern const mozilla::XREAppData* gAppData;
extern bool gSafeMode;
extern bool gFxREmbedded;

extern int gArgc;
extern char** gArgv;
extern int gRestartArgc;
extern char** gRestartArgv;
extern bool gRestartedByOS;
extern bool gLogConsoleErrors;
extern nsString gAbsoluteArgv0Path;

extern bool gIsGtest;

extern bool gKioskMode;
extern int gKioskMonitor;
namespace mozilla {
nsresult AppInfoConstructor(const nsID& aIID, void** aResult);

nsresult MarkProfileEncryptedDatabases();
}  

void BuildCompatVersion(const char* aAppVersion, const char* aAppBuildID,
                        const char* aToolkitBuildID, nsACString& aBuf);

int32_t CompareCompatVersions(const nsACString& aOldCompatVersion,
                              const nsACString& aNewCompatVersion);

nsresult NS_CreateNativeAppSupport(nsINativeAppSupport** aResult);
already_AddRefed<nsINativeAppSupport> NS_GetNativeAppSupport();

#if defined(MOZ_HAS_REMOTE)
already_AddRefed<nsIRemoteService> GetRemoteService();
#endif

nsresult NS_LockProfilePath(nsIFile* aPath, nsIFile* aTempPath,
                            nsIProfileUnlocker** aUnlocker,
                            nsIProfileLock** aResult);

void WriteConsoleLog();

void MozExpectedExit();

class nsINativeAppSupport;

nsresult LaunchChild(bool aBlankCommandLine, bool aTryExec = false);

void UnlockProfile();


namespace mozilla {
namespace startup {
Result<nsCOMPtr<nsIFile>, nsresult> GetIncompleteStartupFile(nsIFile* aProfLD);

void IncreaseDescriptorLimits();
}  

const char* PlatformBuildID();

bool RunningGTest();

}  

void SetupErrorHandling(const char* progname);


bool IsWaylandEnabled();

#endif
