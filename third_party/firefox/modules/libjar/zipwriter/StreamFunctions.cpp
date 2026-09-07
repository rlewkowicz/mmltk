/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "nscore.h"
#include "nsIInputStream.h"
#include "nsIOutputStream.h"

nsresult ZW_ReadData(nsIInputStream* aStream, char* aBuffer, uint32_t aCount) {
  while (aCount > 0) {
    uint32_t read;
    nsresult rv = aStream->Read(aBuffer, aCount, &read);
    NS_ENSURE_SUCCESS(rv, rv);
    aCount -= read;
    aBuffer += read;
    if (read == 0 && aCount > 0) return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsresult ZW_WriteData(nsIOutputStream* aStream, const char* aBuffer,
                      uint32_t aCount) {
  while (aCount > 0) {
    uint32_t written;
    nsresult rv = aStream->Write(aBuffer, aCount, &written);
    NS_ENSURE_SUCCESS(rv, rv);
    if (written <= 0) return NS_ERROR_FAILURE;
    aCount -= written;
    aBuffer += written;
  }

  return NS_OK;
}
