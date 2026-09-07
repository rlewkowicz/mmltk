/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_AvailableMemoryWatcherUtils_h
#define mozilla_AvailableMemoryWatcherUtils_h

#include "nsISupportsUtils.h"  // For nsresult

namespace mozilla {

struct MemoryInfo {
  unsigned long memTotal;
  unsigned long memAvailable;
};
[[maybe_unused]]
static nsresult ReadMemoryFile(const char* meminfoPath, MemoryInfo& aResult) {
  FILE* fd;
  if ((fd = fopen(meminfoPath, "r")) == nullptr) {
    return NS_ERROR_FAILURE;
  }

  char buff[128];

  char namebuffer[20];
  while ((fgets(buff, sizeof(buff), fd)) != nullptr) {
    if (strstr(buff, "MemTotal:")) {
      sscanf(buff, "%s %lu ", namebuffer, &aResult.memTotal);
    }
    if (strstr(buff, "MemAvailable:")) {
      sscanf(buff, "%s %lu ", namebuffer, &aResult.memAvailable);
    }
  }
  fclose(fd);
  return NS_OK;
}

}  

#endif  // ifndef mozilla_AvailableMemoryWatcherUtils_h
