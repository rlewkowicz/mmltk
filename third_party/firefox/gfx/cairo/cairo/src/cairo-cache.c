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

#include "cairoint.h"
#include "cairo-error-private.h"

static void
_cairo_cache_shrink_to_accommodate (cairo_cache_t *cache,
				    unsigned long  additional);

static cairo_bool_t
_cairo_cache_entry_is_non_zero (const void *entry)
{
    return ((const cairo_cache_entry_t *) entry)->size;
}


cairo_status_t
_cairo_cache_init (cairo_cache_t		*cache,
		   cairo_cache_keys_equal_func_t keys_equal,
		   cairo_cache_predicate_func_t  predicate,
		   cairo_destroy_func_t		 entry_destroy,
		   unsigned long		 max_size)
{
    cache->hash_table = _cairo_hash_table_create (keys_equal);
    if (unlikely (cache->hash_table == NULL))
	return _cairo_error (CAIRO_STATUS_NO_MEMORY);

    if (predicate == NULL)
	predicate = _cairo_cache_entry_is_non_zero;
    cache->predicate = predicate;
    cache->entry_destroy = entry_destroy;

    cache->max_size = max_size;
    cache->size = 0;

    cache->freeze_count = 0;

    return CAIRO_STATUS_SUCCESS;
}

static void
_cairo_cache_pluck (void *entry, void *closure)
{
    _cairo_cache_remove (closure, entry);
}

void
_cairo_cache_fini (cairo_cache_t *cache)
{
    _cairo_hash_table_foreach (cache->hash_table,
			       _cairo_cache_pluck,
			       cache);
    assert (cache->size == 0);
    _cairo_hash_table_destroy (cache->hash_table);
}

void
_cairo_cache_freeze (cairo_cache_t *cache)
{
    assert (cache->freeze_count >= 0);

    cache->freeze_count++;
}

void
_cairo_cache_thaw (cairo_cache_t *cache)
{
    assert (cache->freeze_count > 0);

    if (--cache->freeze_count == 0)
	_cairo_cache_shrink_to_accommodate (cache, 0);
}

void *
_cairo_cache_lookup (cairo_cache_t	  *cache,
		     cairo_cache_entry_t  *key)
{
    return _cairo_hash_table_lookup (cache->hash_table,
				     (cairo_hash_entry_t *) key);
}

static cairo_bool_t
_cairo_cache_remove_random (cairo_cache_t *cache)
{
    cairo_cache_entry_t *entry;

    entry = _cairo_hash_table_random_entry (cache->hash_table,
					    cache->predicate);
    if (unlikely (entry == NULL))
	return FALSE;

    _cairo_cache_remove (cache, entry);

    return TRUE;
}

static void
_cairo_cache_shrink_to_accommodate (cairo_cache_t *cache,
				    unsigned long  additional)
{
    while (cache->size + additional > cache->max_size) {
	if (! _cairo_cache_remove_random (cache))
	    return;
    }
}

cairo_status_t
_cairo_cache_insert (cairo_cache_t	 *cache,
		     cairo_cache_entry_t *entry)
{
    cairo_status_t status;

    if (entry->size && ! cache->freeze_count)
	_cairo_cache_shrink_to_accommodate (cache, entry->size);

    status = _cairo_hash_table_insert (cache->hash_table,
				       (cairo_hash_entry_t *) entry);
    if (unlikely (status))
	return status;

    cache->size += entry->size;

    return CAIRO_STATUS_SUCCESS;
}

void
_cairo_cache_remove (cairo_cache_t	 *cache,
		     cairo_cache_entry_t *entry)
{
    cache->size -= entry->size;

    _cairo_hash_table_remove (cache->hash_table,
			      (cairo_hash_entry_t *) entry);

    if (cache->entry_destroy)
	cache->entry_destroy (entry);
}

void
_cairo_cache_foreach (cairo_cache_t		      *cache,
		      cairo_cache_callback_func_t      cache_callback,
		      void			      *closure)
{
    _cairo_hash_table_foreach (cache->hash_table,
			       cache_callback,
			       closure);
}

uintptr_t
_cairo_hash_string (const char *c)
{
    uintptr_t hash = _CAIRO_HASH_INIT_VALUE;
    while (c && *c)
	hash = ((hash << 5) + hash) + *c++;
    return hash;
}

uintptr_t
_cairo_hash_bytes (uintptr_t hash,
		   const void *ptr,
		   unsigned int length)
{
    const uint8_t *bytes = ptr;
    while (length--)
	hash = ((hash << 5) + hash) + *bytes++;
    return hash;
}

uintptr_t
_cairo_hash_uintptr (uintptr_t hash,
                     uintptr_t u)
{
    return _cairo_hash_bytes (hash, &u, sizeof(u));
}
