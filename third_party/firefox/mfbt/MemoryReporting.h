/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_MemoryReporting_h
#define mozilla_MemoryReporting_h

#include <stddef.h>

#ifdef __cplusplus

namespace mozilla {

typedef size_t (*MallocSizeOf)(const void* p);

} 

#endif /* __cplusplus */

typedef size_t (*MozMallocSizeOf)(const void* p);

#endif /* mozilla_MemoryReporting_h */
