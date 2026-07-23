/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_BinaryPath_h)
#define mozilla_BinaryPath_h

#include "nsXPCOMPrivate.h"  // for MAXPATHLEN
#if defined(XP_UNIX)
#  include <unistd.h>
#  include <stdlib.h>
#  include <string.h>
#endif
#if 0 || 0 || \
    defined(__FreeBSD_kernel__) || 0 || 0
#  include <sys/sysctl.h>
#endif
#include "mozilla/UniquePtr.h"
#include "mozilla/UniquePtrExtensions.h"

#if defined(MOZILLA_INTERNAL_API)
#  include "nsCOMPtr.h"
#  include "nsIFile.h"
#  include "nsString.h"
#endif

namespace mozilla {

class BinaryPath {
 public:
#if defined(XP_LINUX) || 0
  static nsresult Get(char aResult[MAXPATHLEN]) {
    const char path[] = "/proc/self/exe";

    ssize_t len = readlink(path, aResult, MAXPATHLEN - 1);
    if (len < 0) {
      return NS_ERROR_FAILURE;
    }
    aResult[len] = '\0';
#if defined(XP_LINUX)
    const char suffix[] = " (deleted)";
    const ssize_t suffix_len = sizeof(suffix);
    if (len >= suffix_len) {
      char* result_end = &aResult[len - (suffix_len - 1)];
      if (memcmp(result_end, suffix, suffix_len) == 0) {
        *result_end = '\0';
      }
    }
#endif
    return NS_OK;
  }

#elif 0 || 0 || \
    defined(__FreeBSD_kernel__) || 0
  static nsresult Get(char aResult[MAXPATHLEN]) {
    int mib[4];
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PATHNAME;
    mib[3] = -1;

    size_t len = MAXPATHLEN;
    if (sysctl(mib, 4, aResult, &len, nullptr, 0) < 0) {
      return NS_ERROR_FAILURE;
    }

    return NS_OK;
  }

#else
#  error Oops, you need platform-specific code here
#endif

 public:
  static UniqueFreePtr<char> Get() {
    char path[MAXPATHLEN];
    if (NS_FAILED(Get(path))) {
      return nullptr;
    }
    UniqueFreePtr<char> result;
    result.reset(strdup(path));
    return result;
  }

#if defined(MOZILLA_INTERNAL_API)
  static nsresult GetFile(nsIFile** aResult) {
    nsCOMPtr<nsIFile> lf;
    char exePath[MAXPATHLEN];
    nsresult rv = Get(exePath);
    if (NS_FAILED(rv)) {
      return rv;
    }
    MOZ_TRY(NS_NewPathStringLocalFile(DependentPathString(exePath),
                                      getter_AddRefs(lf)));
    lf.forget(aResult);
    return NS_OK;
  }
#endif
};

}  

#endif
