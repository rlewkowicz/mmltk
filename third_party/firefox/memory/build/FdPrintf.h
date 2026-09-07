/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(FdPrintf_h_)
#define FdPrintf_h_

#include <cstdarg>

typedef int platform_handle_t;

int VSNPrintf(char* aBuf, std::size_t aSize, const char* aFormat, va_list aArgs)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 0)))
#endif
    ;

int SNPrintf(char* aBuf, std::size_t aSize, const char* aFormat, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

void VFdPrintf(platform_handle_t aFd, const char* aFormat, va_list aArgs)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 0)))
#endif
    ;

void FdPrintf(platform_handle_t aFd, const char* aFormat, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

void FdPuts(platform_handle_t aFd, const char* aBuf, std::size_t aLen);

#endif
