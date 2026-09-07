/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_OPENCLIENTDIRECTORYINFO_H_
#define DOM_QUOTA_OPENCLIENTDIRECTORYINFO_H_

#include <cstdint>

#include "mozilla/dom/quota/ForwardDecls.h"
#include "nsISupportsImpl.h"

namespace mozilla::dom::quota {

class UniversalDirectoryLock;

class OpenClientDirectoryInfo {
 public:
  OpenClientDirectoryInfo();

  ~OpenClientDirectoryInfo();

  void AssertIsOnOwningThread() const;

  void SetFirstAccessPromise(RefPtr<BoolPromise> aFirstAccessPromise);

  RefPtr<BoolPromise> AcquireFirstAccessPromise() const;

  void SetLastAccessDirectoryLock(
      RefPtr<UniversalDirectoryLock> aLastAccessDirectoryLock);

  bool HasLastAccessDirectoryLock() const;

  RefPtr<UniversalDirectoryLock> ForgetLastAccessDirectoryLock();

  uint64_t ClientDirectoryLockHandleCount() const;

  void IncreaseClientDirectoryLockHandleCount();

  void DecreaseClientDirectoryLockHandleCount();

 private:
  NS_DECL_OWNINGTHREAD

  RefPtr<BoolPromise> mFirstAccessPromise;
  RefPtr<UniversalDirectoryLock> mLastAccessDirectoryLock;

  uint64_t mClientDirectoryLockHandleCount = 0;
};

}  

#endif  // DOM_QUOTA_OPENCLIENTDIRECTORYINFO_H_
