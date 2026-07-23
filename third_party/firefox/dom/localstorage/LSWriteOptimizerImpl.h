/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_localstorage_LSWriteOptimizerImpl_h
#define mozilla_dom_localstorage_LSWriteOptimizerImpl_h

#include "LSWriteOptimizer.h"

namespace mozilla::dom {

template <typename T, typename U>
void LSWriteOptimizer<T, U>::InsertItem(const nsAString& aKey, const T& aValue,
                                        int64_t aDelta) {
  AssertIsOnOwningThread();

  mWriteInfos.WithEntryHandle(aKey, [&](auto&& entry) {
    if (entry && entry.Data()->GetType() == WriteInfo::DeleteItem) {

      entry.Update(MakeUnique<UpdateItemInfo>(NextSerialNumber(), aKey, aValue,
                                               true));
    } else {
      entry.InsertOrUpdate(
          MakeUnique<InsertItemInfo>(NextSerialNumber(), aKey, aValue));
    }
  });

  mTotalDelta += aDelta;
}

template <typename T, typename U>
void LSWriteOptimizer<T, U>::UpdateItem(const nsAString& aKey, const T& aValue,
                                        int64_t aDelta) {
  AssertIsOnOwningThread();

  mWriteInfos.WithEntryHandle(aKey, [&](auto&& entry) {
    if (entry && entry.Data()->GetType() == WriteInfo::InsertItem) {
      entry.Update(
          MakeUnique<InsertItemInfo>(NextSerialNumber(), aKey, aValue));
    } else {
      entry.InsertOrUpdate(
          MakeUnique<UpdateItemInfo>(NextSerialNumber(), aKey, aValue,
                                      false));
    }
  });

  mTotalDelta += aDelta;
}

}  

#endif  // mozilla_dom_localstorage_LSWriteOptimizerImpl_h
