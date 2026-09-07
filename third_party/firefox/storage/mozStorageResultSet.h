/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozStorageResultSet_h
#define mozStorageResultSet_h

#include "mozIStorageResultSet.h"
#include "nsCOMArray.h"
class mozIStorageRow;

namespace mozilla {
namespace storage {

class ResultSet final : public mozIStorageResultSet {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_MOZISTORAGERESULTSET

  ResultSet();

  nsresult add(mozIStorageRow* aTuple);

  int32_t rows() const { return mData.Count(); }

 private:
  ~ResultSet();

  int32_t mCurrentIndex;

  nsCOMArray<mozIStorageRow> mData;
};

}  
}  

#endif  // mozStorageResultSet_h
