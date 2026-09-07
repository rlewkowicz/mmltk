/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(XPCOM_IO_FILEPREFERENCES_H_)
#define XPCOM_IO_FILEPREFERENCES_H_

#include "nsAString.h"

namespace mozilla {
namespace FilePreferences {

void InitPrefs();
void InitDirectoriesAllowlist();
bool IsBlockedUNCPath(const nsAString& aFilePath);

bool IsAllowedPath(const nsACString& aFilePath);


extern const char kPathSeparator;

namespace testing {

void SetBlockUNCPaths(bool aBlock);
void AddDirectoryToAllowlist(nsAString const& aPath);
bool NormalizePath(nsAString const& aPath, nsAString& aNormalized);

}  

}  
}  

#endif
