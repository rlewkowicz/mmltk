/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2004 Red Hat, Inc.
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
 * The Initial Developer of the Original Code is Red Hat, Inc.
 *
 * Contributor(s):
 *      Keith Packard <keithp@keithp.com>
 *	Graydon Hoare <graydon@redhat.com>
 *	Carl Worth <cworth@cworth.org>
 */

#ifndef CAIRO_CACHE_PRIVATE_H
#define CAIRO_CACHE_PRIVATE_H

#include "cairo-compiler-private.h"
#include "cairo-types-private.h"

typedef struct _cairo_cache_entry {
    uintptr_t hash;
    unsigned long size;
} cairo_cache_entry_t;

typedef cairo_bool_t (*cairo_cache_predicate_func_t) (const void *entry);

struct _cairo_cache {
    cairo_hash_table_t *hash_table;

    cairo_cache_predicate_func_t predicate;
    cairo_destroy_func_t entry_destroy;

    unsigned long max_size;
    unsigned long size;

    int freeze_count;
};

typedef cairo_bool_t
(*cairo_cache_keys_equal_func_t) (const void *key_a, const void *key_b);

typedef void
(*cairo_cache_callback_func_t) (void *entry,
				void *closure);

cairo_private cairo_status_t
_cairo_cache_init (cairo_cache_t *cache,
	           cairo_cache_keys_equal_func_t keys_equal,
		   cairo_cache_predicate_func_t  predicate,
		   cairo_destroy_func_t	   entry_destroy,
		   unsigned long		   max_size);

cairo_private void
_cairo_cache_fini (cairo_cache_t *cache);

cairo_private void
_cairo_cache_freeze (cairo_cache_t *cache);

cairo_private void
_cairo_cache_thaw (cairo_cache_t *cache);

cairo_private void *
_cairo_cache_lookup (cairo_cache_t	  *cache,
		     cairo_cache_entry_t  *key);

cairo_private cairo_status_t
_cairo_cache_insert (cairo_cache_t	 *cache,
		     cairo_cache_entry_t *entry);

cairo_private void
_cairo_cache_remove (cairo_cache_t	 *cache,
		     cairo_cache_entry_t *entry);

cairo_private void
_cairo_cache_foreach (cairo_cache_t		 *cache,
		      cairo_cache_callback_func_t cache_callback,
		      void			 *closure);

#endif
