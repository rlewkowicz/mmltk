/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MultiInstanceLock.h"

#include "commonupdatedir.h"  // for GetInstallHash
#include "mozilla/UniquePtr.h"
#include "nsPrintfCString.h"
#include "nsPromiseFlatString.h"
#include "nsXULAppAPI.h"
#include "updatedefines.h"  // for NS_t* definitions

#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>



namespace mozilla {

bool GetMultiInstanceLockFileName(const char* nameToken,
                                  const char16_t* installPath,
                                  nsCString& filePath) {
  mozilla::UniquePtr<NS_tchar[]> pathHash;
  if (!GetInstallHash(installPath, pathHash)) {
    return false;
  }

  filePath = nsPrintfCString("/tmp/%s%s-%s", MOZ_APP_VENDOR, nameToken,
                             pathHash.get());


  return true;
}

MultiInstLockHandle OpenMultiInstanceLock(const char* nameToken,
                                          const char16_t* installPath) {
  nsCString filePath;
  if (!GetMultiInstanceLockFileName(nameToken, installPath, filePath)) {
    return MULTI_INSTANCE_LOCK_HANDLE_ERROR;
  }

  int fd = ::open(PromiseFlatCString(filePath).get(),
                  O_CLOEXEC | O_CREAT | O_NOFOLLOW,
                  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
  if (fd != -1) {
    struct flock l = {0};
    l.l_start = 0;
    l.l_len = 0;
    l.l_type = F_RDLCK;
    if (::fcntl(fd, F_SETLK, &l)) {
      ::close(fd);
      fd = -1;
    }
  }
  return fd;

}

void ReleaseMultiInstanceLock(MultiInstLockHandle lock) {
  if (lock != MULTI_INSTANCE_LOCK_HANDLE_ERROR) {
    bool otherInstance = true;
    if (IsOtherInstanceRunning(lock, &otherInstance) && !otherInstance) {
      UniquePtr<NS_tchar[]> linkPath = MakeUnique<NS_tchar[]>(MAXPATHLEN + 1);
      NS_tsnprintf(linkPath.get(), MAXPATHLEN + 1, "/proc/self/fd/%d", lock);
      UniquePtr<NS_tchar[]> lockFilePath =
          MakeUnique<NS_tchar[]>(MAXPATHLEN + 1);
      if (::readlink(linkPath.get(), lockFilePath.get(), MAXPATHLEN + 1) !=
          -1) {
        ::unlink(lockFilePath.get());
      }
    }
    ::close(lock);
  }
}

bool IsOtherInstanceRunning(MultiInstLockHandle lock, bool* aResult) {
  if (lock == MULTI_INSTANCE_LOCK_HANDLE_ERROR) {
    return false;
  }

  struct flock l = {0};
  l.l_start = 0;
  l.l_len = 0;
  l.l_type = F_WRLCK;
  if (::fcntl(lock, F_GETLK, &l)) {
    return false;
  }
  *aResult = l.l_type != F_UNLCK;
  return true;

}

already_AddRefed<nsIFile> GetNormalizedAppFile(nsIFile* aAppFile) {
  nsresult rv;
  nsCOMPtr<nsIFile> appFile;
  if (aAppFile) {
    rv = aAppFile->Clone(getter_AddRefs(appFile));
    NS_ENSURE_SUCCESS(rv, nullptr);
  } else {
    nsCOMPtr<nsIProperties> dirSvc =
        do_GetService(NS_DIRECTORY_SERVICE_CONTRACTID);
    NS_ENSURE_TRUE(dirSvc, nullptr);

    rv = dirSvc->Get(XRE_EXECUTABLE_FILE, NS_GET_IID(nsIFile),
                     getter_AddRefs(appFile));
    NS_ENSURE_SUCCESS(rv, nullptr);
  }


  return appFile.forget();
}

};  
