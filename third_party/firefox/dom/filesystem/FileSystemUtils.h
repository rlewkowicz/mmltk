/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_FileSystemUtils_h
#define mozilla_dom_FileSystemUtils_h

#include "nsIGlobalObject.h"
#include "nsStringFwd.h"
#include "nsTArray.h"

class nsIFile;
class nsIRunnable;

namespace mozilla::dom {

#define FILESYSTEM_DOM_PATH_SEPARATOR_LITERAL "/"
#define FILESYSTEM_DOM_PATH_SEPARATOR_CHAR '/'

class FileSystemUtils {
 public:
  static bool IsValidRelativeDOMPath(const nsAString& aPath,
                                     nsTArray<nsString>& aParts);

  static nsresult DispatchRunnable(nsIGlobalObject* aGlobal,
                                   already_AddRefed<nsIRunnable> aRunnable);
};

}  

#endif  // mozilla_dom_FileSystemUtils_h
