/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozStorageRow.h"
#include "mozStorageResultSet.h"

namespace mozilla {
namespace storage {


ResultSet::ResultSet() : mCurrentIndex(0) {}

ResultSet::~ResultSet() { mData.Clear(); }

nsresult ResultSet::add(mozIStorageRow* aRow) {
  return mData.AppendObject(aRow) ? NS_OK : NS_ERROR_OUT_OF_MEMORY;
}

NS_IMPL_ISUPPORTS(ResultSet, mozIStorageResultSet)


NS_IMETHODIMP
ResultSet::GetNextRow(mozIStorageRow** _row) {
  NS_ENSURE_ARG_POINTER(_row);

  if (mCurrentIndex >= mData.Count()) {
    return NS_OK;
  }

  NS_ADDREF(*_row = mData.ObjectAt(mCurrentIndex++));
  return NS_OK;
}

}  
}  
