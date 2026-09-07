/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_INDEXEDDB_TRANSACTION_OP_RESULT_H_
#define DOM_INDEXEDDB_TRANSACTION_OP_RESULT_H_

#include "nsError.h"
#include "nsString.h"

namespace IPC {

template <typename>
struct ParamTraits;

}  

namespace mozilla::dom::indexedDB {

struct TransactionOpResult {
  nsresult mCode;
  nsCString mErrorMessage;

  MOZ_IMPLICIT TransactionOpResult(nsresult aCode = NS_OK);
  TransactionOpResult(nsresult aCode, const nsACString& aErrorMessage);

  friend struct IPC::ParamTraits<TransactionOpResult>;
};

}  

#endif  // DOM_INDEXEDDB_TRANSACTION_OP_RESULT_H_
