/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_storage_mozStorageAsyncStatement_h_
#define mozilla_storage_mozStorageAsyncStatement_h_

#include "nsString.h"

#include "nsTArray.h"

#include "mozStorageBindingParamsArray.h"
#include "mozStorageStatementData.h"
#include "mozIStorageAsyncStatement.h"
#include "StorageBaseStatementInternal.h"

namespace mozilla {
namespace storage {

class AsyncStatementJSHelper;
class AsyncStatementParamsHolder;
class Connection;

class AsyncStatement final : public mozIStorageAsyncStatement,
                             public StorageBaseStatementInternal {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_MOZISTORAGEASYNCSTATEMENT
  NS_DECL_MOZISTORAGEBASESTATEMENT
  NS_DECL_MOZISTORAGEBINDINGPARAMS
  NS_DECL_STORAGEBASESTATEMENTINTERNAL

  AsyncStatement();

  nsresult initialize(Connection* aDBConnection, sqlite3* aNativeConnection,
                      const nsACString& aSQLStatement);

  inline already_AddRefed<BindingParamsArray> bindingParamsArray() {
    return mParamsArray.forget();
  }

 private:
  ~AsyncStatement();

  mozIStorageBindingParams* getParams();

  nsCString mSQLString;

  RefPtr<BindingParamsArray> mParamsArray;

  nsMainThreadPtrHandle<AsyncStatementParamsHolder> mStatementParamsHolder;

  bool mFinalized;

  friend class AsyncStatementJSHelper;
};

}  
}  

#endif  // mozilla_storage_mozStorageAsyncStatement_h_
