/**************************************************************************
 *
 * Copyright 2008 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


#if !defined(U_STRING_H_)
#define U_STRING_H_

#if !defined(XF86_LIBC_H)
#include <stdio.h>
#endif
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>

#include "util/macros.h" // PRINTFLIKE


#if defined(__cplusplus)
extern "C" {
#endif

#if !defined(_GNU_SOURCE) || 0

#define strchrnul util_strchrnul
static inline char *
util_strchrnul(const char *s, char c)
{
   for (; *s && *s != c; ++s);

   return (char *)s;
}

#endif



#if defined(__cplusplus)
}
#endif

#endif
