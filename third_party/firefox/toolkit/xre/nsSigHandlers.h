/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_XRE_NSSIGHANDLERS_H_
#define TOOLKIT_XRE_NSSIGHANDLERS_H_

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || \
    defined(__i386) || defined(__amd64__)

#  define FPU_EXCEPTION_MASK 0x3f

#  define FPU_STATUS_FLAGS 0xff

#  define SSE_STATUS_FLAGS FPU_EXCEPTION_MASK
#  define SSE_EXCEPTION_MASK (FPU_EXCEPTION_MASK << 7)

#endif

#endif  // TOOLKIT_XRE_NSSIGHANDLERS_H_
