/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_LOCALSTORAGE_LSCIPHERKEYMANAGER_H_
#define DOM_LOCALSTORAGE_LSCIPHERKEYMANAGER_H_

#include "mozilla/dom/quota/CipherKeyManager.h"
#include "mozilla/dom/quota/IPCStreamCipherStrategy.h"

namespace mozilla::dom {

using LSCipherStrategy = quota::IPCStreamCipherStrategy;
using LSCipherKeyManager = quota::CipherKeyManager<LSCipherStrategy>;
using CipherKey = LSCipherStrategy::KeyType;

}  

#endif  // DOM_LOCALSTORAGE_LSCIPHERKEYMANAGER_H_
