/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_PoisonIOInterposer_h)
#define mozilla_PoisonIOInterposer_h

#include "mozilla/Types.h"
#include <stdio.h>

typedef int platform_handle_t;

MOZ_BEGIN_EXTERN_C

void MozillaRegisterDebugHandle(platform_handle_t aHandle);

void MozillaRegisterDebugFD(int aFd);

void MozillaRegisterDebugFILE(FILE* aFile);

void MozillaUnRegisterDebugHandle(platform_handle_t aHandle);

void MozillaUnRegisterDebugFD(int aFd);

void MozillaUnRegisterDebugFILE(FILE* aFile);

MOZ_END_EXTERN_C


#if defined(__cplusplus)
namespace mozilla {
inline bool IsDebugFile(platform_handle_t aFileID) { return true; }
inline void InitPoisonIOInterposer() {}
inline void ClearPoisonIOInterposer() {}
}  
#endif


#endif
