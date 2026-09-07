/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(nsXPCOMPrivate_h_)
#define nsXPCOMPrivate_h_

#include "nscore.h"
#include "nsXPCOM.h"
#include "mozilla/Attributes.h"

#define NS_XPCOM_SHUTDOWN_THREADS_OBSERVER_ID "xpcom-shutdown-threads"

namespace mozilla {

MOZ_CAN_RUN_SCRIPT
nsresult ShutdownXPCOM(nsIServiceManager* aServMgr);

void SetICUMemoryFunctions();

void LogTerm();

}  


#  include <limits.h>  // for PATH_MAX

#  define XPCOM_DLL XUL_DLL

#    define XPCOM_SEARCH_KEY "LD_LIBRARY_PATH"
#    define XUL_DLL "libxul" MOZ_DLL_SUFFIX

#  define GRE_CONF_NAME ".gre.config"
#  define GRE_CONF_PATH "/etc/gre.conf"
#  define GRE_CONF_DIR "/etc/gre.d"
#  define GRE_USER_CONF_DIR ".gre.d"

#if defined(XP_UNIX)
#  define XPCOM_FILE_PATH_SEPARATOR "/"
#  define XPCOM_ENV_PATH_SEPARATOR ":"
#else
#  error need_to_define_your_file_path_separator_and_illegal_characters
#endif

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

extern char16_t* gGREBinPath;

namespace mozilla {
namespace services {

void Shutdown();

}  
}  

#endif
