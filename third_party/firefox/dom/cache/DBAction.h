/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_DBAction_h
#define mozilla_dom_cache_DBAction_h

#include "CacheCipherKeyManager.h"
#include "mozilla/dom/cache/Action.h"
#include "nsString.h"

class mozIStorageConnection;
class nsIFile;

namespace mozilla::dom::cache {

Result<nsCOMPtr<mozIStorageConnection>, nsresult> OpenDBConnection(
    const CacheDirectoryMetadata& aDirectoryMetadata, nsIFile& aDBFile,
    const Maybe<CipherKey>& aMaybeCipherKey);

class DBAction : public Action {
 protected:
  enum Mode { Existing, Create };

  explicit DBAction(Mode aMode);

  virtual ~DBAction();

  virtual void RunWithDBOnTarget(
      SafeRefPtr<Resolver> aResolver,
      const CacheDirectoryMetadata& aDirectoryMetadata, nsIFile* aDBDir,
      mozIStorageConnection* aConn) = 0;

 private:
  void RunOnTarget(SafeRefPtr<Resolver> aResolver,
                   const Maybe<CacheDirectoryMetadata>& aDirectoryMetadata,
                   Data* aOptionalData,
                   const Maybe<CipherKey>& aMaybeCipherKey) override;

  Result<nsCOMPtr<mozIStorageConnection>, nsresult> OpenConnection(
      const CacheDirectoryMetadata& aDirectoryMetadata, nsIFile& aDBDir,
      const Maybe<CipherKey>& aMaybeCipherKey);

  const Mode mMode;
};

class SyncDBAction : public DBAction {
 protected:
  explicit SyncDBAction(Mode aMode);

  virtual ~SyncDBAction();

  virtual nsresult RunSyncWithDBOnTarget(
      const CacheDirectoryMetadata& aDirectoryMetadata, nsIFile* aDBDir,
      mozIStorageConnection* aConn) = 0;

 private:
  virtual void RunWithDBOnTarget(
      SafeRefPtr<Resolver> aResolver,
      const CacheDirectoryMetadata& aDirectoryMetadata, nsIFile* aDBDir,
      mozIStorageConnection* aConn) override;
};

}  

#endif  // mozilla_dom_cache_DBAction_h
