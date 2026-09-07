/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(nsVersionComparator_h_)
#define nsVersionComparator_h_

#include <stdlib.h>
#include <string.h>
#include <assert.h>


namespace mozilla {

int32_t CompareVersions(const char* aStrA, const char* aStrB);


struct Version {
  explicit Version(const char* aVersionString) {
    versionContent = strdup(aVersionString);
  }

  const char* ReadContent() const { return versionContent; }

  ~Version() { free(versionContent); }

  bool operator<(const Version& aRhs) const {
    return CompareVersions(versionContent, aRhs.ReadContent()) < 0;
  }
  bool operator<=(const Version& aRhs) const {
    return CompareVersions(versionContent, aRhs.ReadContent()) < 1;
  }
  bool operator>(const Version& aRhs) const {
    return CompareVersions(versionContent, aRhs.ReadContent()) > 0;
  }
  bool operator>=(const Version& aRhs) const {
    return CompareVersions(versionContent, aRhs.ReadContent()) > -1;
  }
  bool operator==(const Version& aRhs) const {
    return CompareVersions(versionContent, aRhs.ReadContent()) == 0;
  }
  bool operator!=(const Version& aRhs) const {
    return CompareVersions(versionContent, aRhs.ReadContent()) != 0;
  }
  bool operator<(const char* aRhs) const {
    return CompareVersions(versionContent, aRhs) < 0;
  }
  bool operator<=(const char* aRhs) const {
    return CompareVersions(versionContent, aRhs) < 1;
  }
  bool operator>(const char* aRhs) const {
    return CompareVersions(versionContent, aRhs) > 0;
  }
  bool operator>=(const char* aRhs) const {
    return CompareVersions(versionContent, aRhs) > -1;
  }
  bool operator==(const char* aRhs) const {
    return CompareVersions(versionContent, aRhs) == 0;
  }
  bool operator!=(const char* aRhs) const {
    return CompareVersions(versionContent, aRhs) != 0;
  }

 private:
  char* versionContent;
};

}  

#endif
