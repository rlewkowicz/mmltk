/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(mozilla_TaggedAnonymousMemory_h)
#define mozilla_TaggedAnonymousMemory_h


#if defined(__wasi__)
#    include <stdlib.h>
#else
#    include <sys/types.h>
#    include <sys/mman.h>
#endif

#  include "mozilla/Types.h"  // IWYU pragma: keep(MFBT_API)

#if defined(XP_LINUX)

#if defined(__cplusplus)
extern "C" {
#endif

MFBT_API void MozTagAnonymousMemory(const void* aPtr, size_t aLength,
                                    const char* aTag);

MFBT_API void* MozTaggedAnonymousMmap(void* aAddr, size_t aLength, int aProt,
                                      int aFlags, int aFd, off_t aOffset,
                                      const char* aTag);

#if defined(__cplusplus)
}  
#endif

#else

static inline void MozTagAnonymousMemory(const void* aPtr, size_t aLength,
                                         const char* aTag) {}

static inline void* MozTaggedAnonymousMmap(void* aAddr, size_t aLength,
                                           int aProt, int aFlags, int aFd,
                                           off_t aOffset, const char* aTag) {
#if defined(__wasi__)
  MOZ_CRASH("We don't use this memory for WASI right now.");
  return nullptr;
#else
  return mmap(aAddr, aLength, aProt, aFlags, aFd, aOffset);
#endif
}

#endif


#endif
