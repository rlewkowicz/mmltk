/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "UniquePtrExtensions.h"

#include "mozilla/Assertions.h"
#include "mozilla/DebugOnly.h"

#  include <errno.h>
#  include <unistd.h>
#  include <fcntl.h>

namespace mozilla {
namespace detail {

void FileHandleDeleter::operator()(FileHandleHelper aHelper) {
  if (aHelper != nullptr) {
    DebugOnly<bool> ok;
    ok = close(aHelper) == 0 || errno == EINTR;
    MOZ_ASSERT(ok, "failed to close file handle");
  }
}

}  

#if defined(XP_UNIX)
void SetCloseOnExec(detail::FileHandleType aFile) {
  if (aFile >= 0) {
    int fdFlags = fcntl(aFile, F_GETFD);
    MOZ_ASSERT(fdFlags >= 0);
    if (fdFlags >= 0) {
      DebugOnly<int> rv = fcntl(aFile, F_SETFD, fdFlags | FD_CLOEXEC);
      MOZ_ASSERT(rv != -1);
    }
  }
}
#endif

#if !defined(__wasm__)
UniqueFileHandle DuplicateFileHandle(detail::FileHandleType aFile) {
  if (aFile != -1) {
    int fd;
#if defined(F_DUPFD_CLOEXEC)
    fd = fcntl(aFile, F_DUPFD_CLOEXEC, 0);
#else
    fd = dup(aFile);
    SetCloseOnExec(fd);
#endif
    return UniqueFileHandle{fd};
  }
  return nullptr;
}
#endif

}  
