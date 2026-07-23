// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(BASE_PORT_H_)
#define BASE_PORT_H_

#include <stdarg.h>

#if defined(_MSC_VER)
#  define GG_LONGLONG(x) x##I64
#  define GG_ULONGLONG(x) x##UI64
#else
#  define GG_LONGLONG(x) x##LL
#  define GG_ULONGLONG(x) x##ULL
#endif


#define GG_INT8_C(x) (x)
#define GG_INT16_C(x) (x)
#define GG_INT32_C(x) (x)
#define GG_INT64_C(x) GG_LONGLONG(x)

#define GG_UINT8_C(x) (x##U)
#define GG_UINT16_C(x) (x##U)
#define GG_UINT32_C(x) (x##U)
#define GG_UINT64_C(x) GG_ULONGLONG(x)

namespace base {


#if defined(__GNUC__)
#  define base_va_copy(_a, _b) ::va_copy(_a, _b)
#elif defined(_MSC_VER)
#  define base_va_copy(_a, _b) (_a = _b)
#else
#  error No va_copy for your compiler
#endif

}  

#if defined(XP_LINUX) || 0
#  define API_CALL
#endif

#endif
