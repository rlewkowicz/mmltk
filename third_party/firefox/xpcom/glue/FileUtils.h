/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_FileUtils_h)
#define mozilla_FileUtils_h

#include "nscore.h"  // nullptr

#if defined(XP_UNIX)
#  include <unistd.h>
#endif
#include "prio.h"
#include "prlink.h"

#include "nsIFile.h"
#include <errno.h>
#include <limits.h>

namespace mozilla {

typedef int filedesc_t;
typedef const char* pathstr_t;

#if defined(MOZILLA_INTERNAL_API)

struct PRCloseDeleter {
  void operator()(PRFileDesc* aFd) {
    if (aFd) {
      PR_Close(aFd);
    }
  }
};
using AutoFDClose = UniquePtr<PRFileDesc, PRCloseDeleter>;

struct FCloseDeleter {
  void operator()(FILE* p) {
    if (p) {
      fclose(p);
    }
  }
};
using ScopedCloseFile = UniquePtr<FILE, FCloseDeleter>;

bool fallocate(PRFileDesc* aFD, int64_t aLength);

void ReadAheadLib(nsIFile* aFile);

void ReadAheadFile(nsIFile* aFile, const size_t aOffset = 0,
                   const size_t aCount = SIZE_MAX,
                   filedesc_t* aOutFd = nullptr);

PathString GetLibraryName(pathstr_t aDirectory, const char* aLib);
PathString GetLibraryFilePathname(pathstr_t aName, PRFuncPtr aAddr);

#endif

void ReadAheadLib(pathstr_t aFilePath);

void ReadAheadFile(pathstr_t aFilePath, const size_t aOffset = 0,
                   const size_t aCount = SIZE_MAX,
                   filedesc_t* aOutFd = nullptr);

void ReadAhead(filedesc_t aFd, const size_t aOffset = 0,
               const size_t aCount = SIZE_MAX);

}  

#endif
