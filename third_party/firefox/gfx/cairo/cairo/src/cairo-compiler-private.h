/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2002 University of Southern California
 * Copyright © 2005 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 *
 * The Original Code is the cairo graphics library.
 *
 * The Initial Developer of the Original Code is University of Southern
 * California.
 *
 * Contributor(s):
 *	Carl D. Worth <cworth@cworth.org>
 */

#if !defined(CAIRO_COMPILER_PRIVATE_H)
#define CAIRO_COMPILER_PRIVATE_H

#include "cairo.h"

#include "config.h"

#include <stddef.h> /* size_t */
#include <stdint.h> /* SIZE_MAX */

#if !defined(CAIRO_STACK_BUFFER_SIZE)
#define CAIRO_STACK_BUFFER_SIZE (512 * sizeof (int))
#endif

#define CAIRO_STACK_ARRAY_LENGTH(T) (CAIRO_STACK_BUFFER_SIZE / sizeof(T))

#if __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ > 4)
#if defined(__MINGW32__)
#define CAIRO_PRINTF_FORMAT(fmt_index, va_index)                        \
	__attribute__((__format__(__MINGW_PRINTF_FORMAT, fmt_index, va_index)))
#else
#define CAIRO_PRINTF_FORMAT(fmt_index, va_index)                        \
	__attribute__((__format__(__printf__, fmt_index, va_index)))
#endif
#else
#define CAIRO_PRINTF_FORMAT(fmt_index, va_index)
#endif

#define CAIRO_HAS_HIDDEN_SYMBOLS 1
#if (__GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 3)) && \
    (defined(__ELF__) || 0) &&			\
    !0
#define cairo_private_no_warn	__attribute__((__visibility__("hidden")))
#elif defined(__SUNPRO_C) && (__SUNPRO_C >= 0x550)
#define cairo_private_no_warn	__hidden
#else
#define cairo_private_no_warn
#undef CAIRO_HAS_HIDDEN_SYMBOLS
#endif

#if !defined(WARN_UNUSED_RESULT)
#define WARN_UNUSED_RESULT
#endif

#define cairo_warn	    WARN_UNUSED_RESULT
#define cairo_private	    cairo_private_no_warn cairo_warn

#if __GNUC__ >= 2 && defined(__ELF__)
# define CAIRO_FUNCTION_ALIAS(old, new)		\
	extern __typeof (new) old		\
	__asm__ ("" #old)			\
	__attribute__((__alias__("" #new)))
#else
# define CAIRO_FUNCTION_ALIAS(old, new)
#endif

#if __GNUC__ >= 3
#define cairo_pure __attribute__((pure))
#define cairo_const __attribute__((const))
#define cairo_always_inline inline __attribute__((always_inline))
#else
#define cairo_pure
#define cairo_const
#define cairo_always_inline inline
#endif

#if defined(__GNUC__) && (__GNUC__ > 2) && defined(__OPTIMIZE__)
#define likely(expr) (__builtin_expect (!!(expr), 1))
#define unlikely(expr) (__builtin_expect (!!(expr), 0))
#else
#define likely(expr) (expr)
#define unlikely(expr) (expr)
#endif

#if !defined(__GNUC__) && !defined (__clang__)
#undef __attribute__
#define __attribute__(x)
#endif

#if (0 && !defined(__WINE__)) || defined(_MSC_VER)
#define access _access
#if !defined(R_OK)
#define R_OK 4
#endif
#define fdopen _fdopen
#define hypot _hypot
#define pclose _pclose
#define popen _popen
#define strdup _strdup
#define unlink _unlink
#if _MSC_VER < 1900
  #define vsnprintf _vsnprintf
  #define snprintf _snprintf
#endif
#endif

#if defined(_MSC_VER)
#if !defined(__cplusplus)
#undef inline
#define inline __inline
#endif
#endif

#if defined(_MSC_VER) && defined(_M_IX86)
#define CAIRO_ENSURE_UNIQUE                       \
    do {                                          \
	char file[] = __FILE__;                   \
	__asm {                                   \
	    __asm jmp __internal_skip_line_no     \
	    __asm _emit (__COUNTER__ & 0xff)      \
	    __asm _emit ((__COUNTER__>>8) & 0xff) \
	    __asm _emit ((__COUNTER__>>16) & 0xff)\
	    __asm _emit ((__COUNTER__>>24) & 0xff)\
	    __asm lea eax, dword ptr file         \
	    __asm __internal_skip_line_no:        \
	};                                        \
    } while (0)
#else
#define CAIRO_ENSURE_UNIQUE    do { } while (0)
#endif

#if defined(__STRICT_ANSI__)
#undef inline
#define inline __inline__
#endif

static cairo_always_inline cairo_bool_t
_cairo_fallback_add_size_t_overflow(size_t a, size_t b, size_t *c)
{
    if (b > SIZE_MAX - a)
        return 1;

    *c = a + b;
    return 0;
}

static cairo_always_inline cairo_bool_t
_cairo_fallback_mul_size_t_overflow(size_t a, size_t b, size_t *c)
{
    if (b != 0 && a > SIZE_MAX / b)
        return 1;

    *c = a * b;
    return 0;
}

#if defined(__clang__)
#if defined(__has_builtin) && __has_builtin(__builtin_add_overflow)
#define _cairo_add_size_t_overflow(a, b, c)  __builtin_add_overflow((size_t)(a), (size_t)(b), (size_t*)(c))
#define _cairo_mul_size_t_overflow(a, b, c)  __builtin_mul_overflow((size_t)(a), (size_t)(b), (size_t*)(c))
#endif
#elif __GNUC__ >= 8 || (__GNUC__ >= 5 && (INTPTR_MAX == INT64_MAX))
#define _cairo_add_size_t_overflow(a, b, c)  __builtin_add_overflow((size_t)(a), (size_t)(b), (size_t*)(c))
#define _cairo_mul_size_t_overflow(a, b, c)  __builtin_mul_overflow((size_t)(a), (size_t)(b), (size_t*)(c))
#elif defined(_MSC_VER) && defined(HAVE_INTSAFE_H)
#include <intsafe.h>
#define _cairo_add_size_t_overflow(a,b,c) (SizeTAdd((size_t)(a), (size_t)(b), (size_t*)(c)) != S_OK)
#define _cairo_mul_size_t_overflow(a,b,c) (SizeTMult((size_t)(a), (size_t)(b), (size_t*)(c)) != S_OK)
#endif

#if !defined(_cairo_add_size_t_overflow)
#define _cairo_add_size_t_overflow _cairo_fallback_add_size_t_overflow
#define _cairo_mul_size_t_overflow _cairo_fallback_mul_size_t_overflow
#endif

#endif
