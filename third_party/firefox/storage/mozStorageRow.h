/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozStorageRow_h
#define mozStorageRow_h

#include "mozIStorageRow.h"
#include "nsCOMArray.h"
#include "nsTHashMap.h"
class nsIVariant;
struct sqlite3_stmt;

namespace mozilla {
namespace storage {

class Row final : public mozIStorageRow {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_MOZISTORAGEROW
  NS_DECL_MOZISTORAGEVALUEARRAY

  Row() : mNumCols(0) {}

  nsresult initialize(sqlite3_stmt* aStatement);

 private:
  ~Row() = default;

  uint32_t mNumCols;

  nsCOMArray<nsIVariant> mData;

  nsTHashMap<nsCStringHashKey, uint32_t> mNameHashtable;
};

}  
}  

#endif  // mozStorageRow_h
