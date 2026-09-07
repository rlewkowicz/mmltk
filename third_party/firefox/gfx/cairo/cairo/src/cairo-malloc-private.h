/* Cairo - a vector graphics library with display and print output
 *
 * Copyright © 2007 Mozilla Corporation
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
 * The Initial Developer of the Original Code is Mozilla Foundation
 *
 * Contributor(s):
 *	Vladimir Vukicevic <vladimir@pobox.com>
 */

#ifndef CAIRO_MALLOC_PRIVATE_H
#define CAIRO_MALLOC_PRIVATE_H

#include "cairo-wideint-private.h"
#include <stdlib.h>

#if HAVE_MEMFAULT
#include <memfault.h>
#define CAIRO_INJECT_FAULT() MEMFAULT_INJECT_FAULT()
#else
#define CAIRO_INJECT_FAULT() 0
#endif


#define _cairo_malloc(size) \
   ((size) != 0 ? malloc(size) : NULL)


#define _cairo_calloc(size) \
    ((size) != 0 ? calloc(1,size) : NULL)


static cairo_always_inline void *
_cairo_malloc_ab(size_t a, size_t size)
{
    size_t c;
    if (_cairo_mul_size_t_overflow (a, size, &c))
	return NULL;

    return _cairo_malloc(c);
}


static cairo_always_inline void *
_cairo_calloc_ab(size_t a, size_t size)
{
    size_t c;
    if (_cairo_mul_size_t_overflow (a, size, &c))
	return NULL;

    return _cairo_calloc(c);
}


static cairo_always_inline void *
_cairo_realloc_ab(void *ptr, size_t a, size_t size)
{
    size_t c;
    if (_cairo_mul_size_t_overflow (a, size, &c))
	return NULL;

    return realloc(ptr, c);
}


static cairo_always_inline void *
_cairo_malloc_abc(size_t a, size_t b, size_t size)
{
    size_t c, d;
    if (_cairo_mul_size_t_overflow (a, b, &c))
	return NULL;

    if (_cairo_mul_size_t_overflow (c, size, &d))
	return NULL;

    return _cairo_malloc(d);
}


static cairo_always_inline void *
_cairo_malloc_ab_plus_c(size_t a, size_t size, size_t c)
{
    size_t d, e;
    if (_cairo_mul_size_t_overflow (a, size, &d))
	return NULL;

    if (_cairo_add_size_t_overflow (d, c, &e))
	return NULL;

    return _cairo_malloc(e);
}

#endif /* CAIRO_MALLOC_PRIVATE_H */
