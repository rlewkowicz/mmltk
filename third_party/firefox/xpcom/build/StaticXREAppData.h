/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef StaticXREAppData_h
#define StaticXREAppData_h

#include <stdint.h>

namespace mozilla {

#define NS_XRE_ENABLE_PROFILE_MIGRATOR (1 << 1)

struct StaticXREAppData {
  const char* vendor;
  const char* name;
  const char* remotingName;
  const char* version;
  const char* buildID;
  const char* ID;
  const char* copyright;
  uint32_t flags;
  const char* minVersion;
  const char* maxVersion;
  const char* profile;
  const char* UAName;
  const char* sourceURL;
  const char* sourceRevision;
  const char* updateURL;
};

}  

#endif  // StaticXREAppData_h
