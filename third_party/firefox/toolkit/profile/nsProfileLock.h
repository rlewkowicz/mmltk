/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(_nsProfileLock_h_)
#define _nsProfileLock_h_

#include "nsIFile.h"

class nsIProfileUnlocker;


#if defined(XP_UNIX)
#  include <signal.h>
#  include "prclist.h"
#endif

class nsProfileLock
#if defined(XP_UNIX)
    : public PRCList
#endif
{
 public:
  nsProfileLock();
  nsProfileLock(nsProfileLock& src);

  ~nsProfileLock();

  nsProfileLock& operator=(nsProfileLock& rhs);

  nsresult Lock(nsIFile* aProfileDir, nsIProfileUnlocker** aUnlocker);

  nsresult Unlock(bool aFatalSignal = false);

  static bool IsMaybeLockFile(nsIFile* aFile);

  nsresult Cleanup();

  nsresult GetReplacedLockTime(PRTime* aResult);

 private:
  bool mHaveLock;
  PRTime mReplacedLockTime;
  nsCOMPtr<nsIFile> mLockFile;

#if defined(XP_UNIX)

  struct RemovePidLockFilesExiting {
    RemovePidLockFilesExiting() = default;
    ~RemovePidLockFilesExiting() { RemovePidLockFiles(false); }
  };

  static void RemovePidLockFiles(bool aFatalSignal);
  static void FatalSignalHandler(int signo
#if defined(SA_SIGINFO)
                                 ,
                                 siginfo_t* info, void* context
#endif
  );
  static PRCList mPidLockList;

  nsresult LockWithFcntl(nsIFile* aLockFile,
                         nsIProfileUnlocker** aUnlocker = nullptr);

  nsresult LockWithSymlink(nsIFile* aLockFile, bool aHaveFcntlLock);

  char* mPidLockFileName;
  int mLockFileDesc;
#endif
};

#endif
