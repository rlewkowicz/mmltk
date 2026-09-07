/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef crc32c_h
#define crc32c_h

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t ComputeCrc32c(uint32_t aCrc, const void* aBuf, size_t aSize);

#ifdef __cplusplus
}  
#endif

#endif  // crc32c_h
