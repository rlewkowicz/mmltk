/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_INDEXEDDB_INDEXEDDBCIPHERKEYMANAGER_H_
#define DOM_INDEXEDDB_INDEXEDDBCIPHERKEYMANAGER_H_

#include "mozilla/dom/quota/CipherKeyManager.h"
#include "mozilla/dom/quota/IPCStreamCipherStrategy.h"

namespace mozilla::dom {


using IndexedDBCipherStrategy = mozilla::dom::quota::IPCStreamCipherStrategy;
using IndexedDBCipherKeyManager =
    mozilla::dom::quota::CipherKeyManager<IndexedDBCipherStrategy>;
using CipherKey = IndexedDBCipherStrategy::KeyType;

}  

#endif  // DOM_INDEXEDDB_INDEXEDDBCIPHERKEYMANAGER_H_
