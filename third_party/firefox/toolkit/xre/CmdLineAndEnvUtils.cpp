/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsCOMPtr.h"
#include "nsIFile.h"
#include "nsDependentString.h"
#include "prenv.h"


namespace mozilla {

already_AddRefed<nsIFile> GetFileFromEnv(const char* name) {
  nsresult rv;
  nsCOMPtr<nsIFile> file;

  const char* arg = PR_GetEnv(name);
  if (!arg || !*arg) {
    return nullptr;
  }

  rv = NS_NewNativeLocalFile(nsDependentCString(arg), getter_AddRefs(file));
  if (NS_FAILED(rv)) {
    return nullptr;
  }

  return file.forget();
}

}  
