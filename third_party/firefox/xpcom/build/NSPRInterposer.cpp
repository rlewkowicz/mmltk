/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IOInterposer.h"
#include "NSPRInterposer.h"

#include "prio.h"
#include "private/pprio.h"
#include "nsDebug.h"
#include "nscore.h"

#include <sys/param.h>
#  include "prprf.h"
#  include <unistd.h>

namespace {

PRCloseFN sCloseFn = nullptr;
PRReadFN sReadFn = nullptr;
PRWriteFN sWriteFn = nullptr;
PRFsyncFN sFSyncFn = nullptr;
PRFileInfoFN sFileInfoFn = nullptr;
PRFileInfo64FN sFileInfo64Fn = nullptr;

static int32_t GetPathFromFd(int32_t aFd, char* aBuf, size_t aBufSize) {
  char procPath[32];
  if (PR_snprintf(procPath, sizeof(procPath), "/proc/self/fd/%i", aFd) ==
      (PRUint32)-1) {
    return -1;
  }

  int32_t ret = readlink(procPath, aBuf, aBufSize - 1);
  if (ret > -1) {
    aBuf[ret] = '\0';
  }

  return ret;
}

class NSPRIOAutoObservation : public mozilla::IOInterposeObserver::Observation {
 public:
  explicit NSPRIOAutoObservation(mozilla::IOInterposeObserver::Operation aOp,
                                 PRFileDesc* aFd)
      : mozilla::IOInterposeObserver::Observation(aOp, "NSPRIOInterposer") {
    char filename[MAXPATHLEN];
    if (mShouldReport && aFd &&
        GetPathFromFd(PR_FileDesc2NativeHandle(aFd), filename,
                      sizeof(filename)) != -1) {
      CopyUTF8toUTF16(mozilla::MakeStringSpan(filename), mFilename);
    } else {
      mFilename.Truncate();
    }
  }

  void Filename(nsAString& aFilename) override { aFilename = mFilename; }

  ~NSPRIOAutoObservation() override { Report(); }

 private:
  nsString mFilename;
};

PRStatus PR_CALLBACK interposedClose(PRFileDesc* aFd) {
  NS_ASSERTION(sCloseFn, "NSPR IO Interposing: sCloseFn is NULL");

  NSPRIOAutoObservation timer(mozilla::IOInterposeObserver::OpClose, aFd);
  return sCloseFn(aFd);
}

int32_t PR_CALLBACK interposedRead(PRFileDesc* aFd, void* aBuf, int32_t aAmt) {
  NS_ASSERTION(sReadFn, "NSPR IO Interposing: sReadFn is NULL");

  NSPRIOAutoObservation timer(mozilla::IOInterposeObserver::OpRead, aFd);
  return sReadFn(aFd, aBuf, aAmt);
}

int32_t PR_CALLBACK interposedWrite(PRFileDesc* aFd, const void* aBuf,
                                    int32_t aAmt) {
  NS_ASSERTION(sWriteFn, "NSPR IO Interposing: sWriteFn is NULL");

  NSPRIOAutoObservation timer(mozilla::IOInterposeObserver::OpWrite, aFd);
  return sWriteFn(aFd, aBuf, aAmt);
}

PRStatus PR_CALLBACK interposedFSync(PRFileDesc* aFd) {
  NS_ASSERTION(sFSyncFn, "NSPR IO Interposing: sFSyncFn is NULL");

  NSPRIOAutoObservation timer(mozilla::IOInterposeObserver::OpFSync, aFd);
  return sFSyncFn(aFd);
}

PRStatus PR_CALLBACK interposedFileInfo(PRFileDesc* aFd, PRFileInfo* aInfo) {
  NS_ASSERTION(sFileInfoFn, "NSPR IO Interposing: sFileInfoFn is NULL");

  NSPRIOAutoObservation timer(mozilla::IOInterposeObserver::OpStat, aFd);
  return sFileInfoFn(aFd, aInfo);
}

PRStatus PR_CALLBACK interposedFileInfo64(PRFileDesc* aFd,
                                          PRFileInfo64* aInfo) {
  NS_ASSERTION(sFileInfo64Fn, "NSPR IO Interposing: sFileInfo64Fn is NULL");

  NSPRIOAutoObservation timer(mozilla::IOInterposeObserver::OpStat, aFd);
  return sFileInfo64Fn(aFd, aInfo);
}

}  

namespace mozilla {

void InitNSPRIOInterposing() {
  MOZ_ASSERT(!sCloseFn && !sReadFn && !sWriteFn && !sFSyncFn && !sFileInfoFn &&
             !sFileInfo64Fn);


  PRIOMethods* methods = const_cast<PRIOMethods*>(PR_GetFileMethods());

  MOZ_ASSERT(methods);
  if (!methods) {
    return;
  }

  sCloseFn = methods->close;
  sReadFn = methods->read;
  sWriteFn = methods->write;
  sFSyncFn = methods->fsync;
  sFileInfoFn = methods->fileInfo;
  sFileInfo64Fn = methods->fileInfo64;

  methods->close = &interposedClose;
  methods->read = &interposedRead;
  methods->write = &interposedWrite;
  methods->fsync = &interposedFSync;
  methods->fileInfo = &interposedFileInfo;
  methods->fileInfo64 = &interposedFileInfo64;
}

void ClearNSPRIOInterposing() {
  MOZ_ASSERT(sCloseFn && sReadFn && sWriteFn && sFSyncFn && sFileInfoFn &&
             sFileInfo64Fn);

  PRIOMethods* methods = const_cast<PRIOMethods*>(PR_GetFileMethods());

  MOZ_ASSERT(methods);
  if (!methods) {
    return;
  }

  methods->close = sCloseFn;
  methods->read = sReadFn;
  methods->write = sWriteFn;
  methods->fsync = sFSyncFn;
  methods->fileInfo = sFileInfoFn;
  methods->fileInfo64 = sFileInfo64Fn;

  sCloseFn = nullptr;
  sReadFn = nullptr;
  sWriteFn = nullptr;
  sFSyncFn = nullptr;
  sFileInfoFn = nullptr;
  sFileInfo64Fn = nullptr;
}

}  
