/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_QuotaClient_h
#define mozilla_dom_cache_QuotaClient_h

#include "mozIStorageConnection.h"
#include "mozilla/dom/cache/Types.h"
#include "mozilla/dom/quota/Client.h"

namespace mozilla::dom::cache {

already_AddRefed<quota::Client> CreateQuotaClient();


nsresult RestorePaddingFile(nsIFile* aBaseDir, mozIStorageConnection* aConn);

nsresult WipePaddingFile(const CacheDirectoryMetadata& aDirectoryMetadata,
                         nsIFile* aBaseDir);

extern const nsLiteralString kCachesSQLiteFilename;

}  

#endif  // mozilla_dom_cache_QuotaClient_h
