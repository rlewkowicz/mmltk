/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozStorageError.h"

namespace mozilla {
namespace storage {


Error::Error(int aResult, const char* aMessage)
    : mResult(aResult), mMessage(aMessage) {}

NS_IMPL_ISUPPORTS(Error, mozIStorageError)


NS_IMETHODIMP
Error::GetResult(int32_t* _result) {
  *_result = mResult;
  return NS_OK;
}

NS_IMETHODIMP
Error::GetMessage(nsACString& _message) {
  _message = mMessage;
  return NS_OK;
}

}  
}  
