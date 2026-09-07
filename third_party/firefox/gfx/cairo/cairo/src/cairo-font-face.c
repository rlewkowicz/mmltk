/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2002 University of Southern California
 * Copyright © 2005 Red Hat Inc.
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
 *      Graydon Hoare <graydon@redhat.com>
 *      Owen Taylor <otaylor@redhat.com>
 */

#include "cairoint.h"
#include "cairo-error-private.h"



const cairo_font_face_t _cairo_font_face_nil = {
    { 0 },				
    CAIRO_STATUS_NO_MEMORY,		
    CAIRO_REFERENCE_COUNT_INVALID,	
    { 0, 0, 0, NULL },			
    NULL
};
const cairo_font_face_t _cairo_font_face_nil_file_not_found = {
    { 0 },				
    CAIRO_STATUS_FILE_NOT_FOUND,	
    CAIRO_REFERENCE_COUNT_INVALID,	
    { 0, 0, 0, NULL },			
    NULL
};

cairo_status_t
_cairo_font_face_set_error (cairo_font_face_t *font_face,
	                    cairo_status_t     status)
{
    if (status == CAIRO_STATUS_SUCCESS)
	return status;

    _cairo_status_set_error (&font_face->status, status);

    return _cairo_error (status);
}

void
_cairo_font_face_init (cairo_font_face_t               *font_face,
		       const cairo_font_face_backend_t *backend)
{
    CAIRO_MUTEX_INITIALIZE ();

    font_face->status = CAIRO_STATUS_SUCCESS;
    CAIRO_REFERENCE_COUNT_INIT (&font_face->ref_count, 1);
    font_face->backend = backend;

    _cairo_user_data_array_init (&font_face->user_data);
}

cairo_font_face_t *
cairo_font_face_reference (cairo_font_face_t *font_face)
{
    if (font_face == NULL ||
	CAIRO_REFERENCE_COUNT_IS_INVALID (&font_face->ref_count))
	return font_face;


    _cairo_reference_count_inc (&font_face->ref_count);

    return font_face;
}

static inline cairo_bool_t
__put(cairo_reference_count_t *v)
{
    int c, old;

    c = CAIRO_REFERENCE_COUNT_GET_VALUE(v);
    while (c != 1 && (old = _cairo_atomic_int_cmpxchg_return_old(&v->ref_count, c, c - 1)) != c)
	c = old;

    return c != 1;
}

cairo_bool_t
_cairo_font_face_destroy (void *abstract_face)
{
#if 0 /* Nothing needs to be done, we can just drop the last reference */
    cairo_font_face_t *font_face = abstract_face;
    return _cairo_reference_count_dec_and_test (&font_face->ref_count);
#endif
    return TRUE;
}

void
cairo_font_face_destroy (cairo_font_face_t *font_face)
{
    if (font_face == NULL ||
	CAIRO_REFERENCE_COUNT_IS_INVALID (&font_face->ref_count))
	return;

    assert (CAIRO_REFERENCE_COUNT_HAS_REFERENCE (&font_face->ref_count));

    if (__put (&font_face->ref_count))
	return;

    if (! font_face->backend->destroy (font_face))
	return;

    _cairo_user_data_array_fini (&font_face->user_data);

    free (font_face);
}

cairo_font_type_t
cairo_font_face_get_type (cairo_font_face_t *font_face)
{
    if (CAIRO_REFERENCE_COUNT_IS_INVALID (&font_face->ref_count))
	return CAIRO_FONT_TYPE_TOY;

    return font_face->backend->type;
}

unsigned int
cairo_font_face_get_reference_count (cairo_font_face_t *font_face)
{
    if (font_face == NULL ||
	CAIRO_REFERENCE_COUNT_IS_INVALID (&font_face->ref_count))
	return 0;

    return CAIRO_REFERENCE_COUNT_GET_VALUE (&font_face->ref_count);
}

cairo_status_t
cairo_font_face_status (cairo_font_face_t *font_face)
{
    return font_face->status;
}

void *
cairo_font_face_get_user_data (cairo_font_face_t	   *font_face,
			       const cairo_user_data_key_t *key)
{
    return _cairo_user_data_array_get_data (&font_face->user_data,
					    key);
}

cairo_status_t
cairo_font_face_set_user_data (cairo_font_face_t	   *font_face,
			       const cairo_user_data_key_t *key,
			       void			   *user_data,
			       cairo_destroy_func_t	    destroy)
{
    if (CAIRO_REFERENCE_COUNT_IS_INVALID (&font_face->ref_count))
	return font_face->status;

    return _cairo_user_data_array_set_data (&font_face->user_data,
					    key, user_data, destroy);
}

void
_cairo_unscaled_font_init (cairo_unscaled_font_t               *unscaled_font,
			   const cairo_unscaled_font_backend_t *backend)
{
    CAIRO_REFERENCE_COUNT_INIT (&unscaled_font->ref_count, 1);
    unscaled_font->backend = backend;
}

cairo_unscaled_font_t *
_cairo_unscaled_font_reference (cairo_unscaled_font_t *unscaled_font)
{
    if (unscaled_font == NULL)
	return NULL;

    assert (CAIRO_REFERENCE_COUNT_HAS_REFERENCE (&unscaled_font->ref_count));

    _cairo_reference_count_inc (&unscaled_font->ref_count);

    return unscaled_font;
}

void
_cairo_unscaled_font_destroy (cairo_unscaled_font_t *unscaled_font)
{
    if (unscaled_font == NULL)
	return;

    assert (CAIRO_REFERENCE_COUNT_HAS_REFERENCE (&unscaled_font->ref_count));

    if (__put (&unscaled_font->ref_count))
	return;

    if (! unscaled_font->backend->destroy (unscaled_font))
	return;

    free (unscaled_font);
}
