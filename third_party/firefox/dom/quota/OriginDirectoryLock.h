/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_ORIGINDIRECTORYLOCK_H_
#define DOM_QUOTA_ORIGINDIRECTORYLOCK_H_

#include "mozilla/dom/quota/DirectoryLockImpl.h"
#include "mozilla/dom/quota/PersistenceType.h"
#include "nsStringFwd.h"

template <class T>
class RefPtr;

namespace mozilla {

template <typename T>
class MovingNotNull;

}  

namespace mozilla::dom::quota {

struct OriginMetadata;
class QuotaManager;

class OriginDirectoryLock : public DirectoryLockImpl {
  friend class QuotaManager;

 public:
  using DirectoryLockImpl::DirectoryLockImpl;


  PersistenceType GetPersistenceType() const {
    return DirectoryLockImpl::GetPersistenceType();
  }

  quota::OriginMetadata OriginMetadata() const {
    return DirectoryLockImpl::OriginMetadata();
  }

  const nsACString& Origin() const { return DirectoryLockImpl::Origin(); }

 private:
  static RefPtr<OriginDirectoryLock> CreateForEviction(
      MovingNotNull<RefPtr<QuotaManager>> aQuotaManager,
      PersistenceType aPersistenceType,
      const quota::OriginMetadata& aOriginMetadata);
};

}  

#endif  // DOM_QUOTA_ORIGINDIRECTORYLOCK_H_
