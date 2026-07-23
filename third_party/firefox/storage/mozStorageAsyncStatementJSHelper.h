/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_storage_mozStorageAsyncStatementJSHelper_h_
#define mozilla_storage_mozStorageAsyncStatementJSHelper_h_

#include "nsIXPCScriptable.h"

namespace mozilla {
namespace storage {

class AsyncStatement;
class AsyncStatementParams;

class AsyncStatementJSHelper : public nsIXPCScriptable {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIXPCSCRIPTABLE

 private:
  nsresult getParams(AsyncStatement*, JSContext*, JSObject*, JS::Value*);
};

class AsyncStatementParamsHolder final : public nsISupports {
 public:
  NS_DECL_ISUPPORTS

  explicit AsyncStatementParamsHolder(AsyncStatementParams* aParams)
      : mParams(aParams) {}

  AsyncStatementParams* Get() const {
    MOZ_ASSERT(mParams);
    return mParams;
  }

 private:
  virtual ~AsyncStatementParamsHolder();

  RefPtr<AsyncStatementParams> mParams;
};

}  
}  

#endif  // mozilla_storage_mozStorageAsyncStatementJSHelper_h_
