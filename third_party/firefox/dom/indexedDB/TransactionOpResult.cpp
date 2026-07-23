/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TransactionOpResult.h"

#include "IndexedDBCommon.h"

namespace mozilla::dom::indexedDB {

TransactionOpResult::TransactionOpResult(nsresult aCode)
    : mCode(ClampResultCode(aCode)) {}

TransactionOpResult::TransactionOpResult(nsresult aCode,
                                         const nsACString& aErrorMessage)
    : mCode(ClampResultCode(aCode)), mErrorMessage(aErrorMessage) {
  MOZ_ASSERT(NS_FAILED(aCode),
             "TransactionOpResult's constructor with error message shall be "
             "used only if aCode represents an error");
}

}  
