/**************************************************************************
 *
 * Copyright 2007-2013 VMware, Inc.
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

#include "no_extern_c.h"

#if !defined(_C99_COMPAT_H_)
#define _C99_COMPAT_H_


#if defined(_MSC_VER)

#if _MSC_VER < 1900
#    error "Microsoft Visual Studio 2015 or higher required"
#endif

#if !defined(_ALLOW_KEYWORD_MACROS)
#    define _ALLOW_KEYWORD_MACROS
#endif

#  include <crtdefs.h>
#  undef _CRTRESTRICT
#  define _CRTRESTRICT
#endif


#if !defined(inline)
#if defined(__cplusplus)
#elif defined(__GNUC__)
#    define inline __inline__
#elif defined(_MSC_VER)
#    define inline __inline
#elif defined(__ICL)
#    define inline __inline
#elif defined(__INTEL_COMPILER)
#elif defined(__WATCOMC__) && (__WATCOMC__ >= 1100)
#    define inline __inline
#elif (__STDC_VERSION__ >= 199901L)
#else
#    define inline
#endif
#endif


#if !defined(restrict)
#if (__STDC_VERSION__ >= 199901L) && !defined(__cplusplus)
#elif defined(__GNUC__)
#    define restrict __restrict__
#elif defined(_MSC_VER)
#    define restrict __restrict
#else
#    define restrict /* */
#endif
#endif


#if !defined(__func__)
#if (__STDC_VERSION__ >= 199901L)
#elif defined(__GNUC__)
#    define __func__ __FUNCTION__
#elif defined(_MSC_VER)
#    define __func__ __FUNCTION__
#else
#    define __func__ "<unknown>"
#endif
#endif




#if defined(HAVE_SCONS)

#    define HAVE_PTHREAD
#    define HAVE_POSIX_MEMALIGN

#if defined(__GNUC__)
#if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 2)
#      error "GCC version 4.2 or higher required"
#endif

#    define HAVE___BUILTIN_CLZ 1
#    define HAVE___BUILTIN_CLZLL 1
#    define HAVE___BUILTIN_CTZ 1
#    define HAVE___BUILTIN_EXPECT 1
#    define HAVE___BUILTIN_FFS 1
#    define HAVE___BUILTIN_FFSLL 1
#    define HAVE___BUILTIN_POPCOUNT 1
#    define HAVE___BUILTIN_POPCOUNTLL 1
#    define HAVE_FUNC_ATTRIBUTE_FLATTEN 1
#    define HAVE_FUNC_ATTRIBUTE_UNUSED 1
#    define HAVE_FUNC_ATTRIBUTE_FORMAT 1
#    define HAVE_FUNC_ATTRIBUTE_PACKED 1
#    define HAVE_FUNC_ATTRIBUTE_ALIAS 1
#    define HAVE_FUNC_ATTRIBUTE_NORETURN 1

#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3)
#      define HAVE___BUILTIN_BSWAP32 1
#      define HAVE___BUILTIN_BSWAP64 1
#endif

#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5)
#      define HAVE___BUILTIN_UNREACHABLE 1
#endif

#endif

#endif


#endif
