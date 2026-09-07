/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2005 Red Hat, Inc
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
 *	Carl Worth <cworth@cworth.org>
 */

#ifndef CAIRO_PAGINATED_H
#define CAIRO_PAGINATED_H

#include "cairoint.h"

struct _cairo_paginated_surface_backend {
    cairo_warn cairo_int_status_t
    (*start_page)		(void			*surface);

    cairo_warn cairo_int_status_t
    (*set_paginated_mode)	(void			*surface,
				 cairo_paginated_mode_t	 mode);

    cairo_warn cairo_int_status_t
    (*set_bounding_box)	(void		*surface,
			 cairo_box_t	*bbox);

    cairo_warn cairo_int_status_t
    (*set_fallback_images_required) (void	    *surface,
				     cairo_bool_t    fallbacks_required);

    cairo_bool_t
    (*supports_fine_grained_fallbacks) (void	    *surface);

    cairo_bool_t
    (*requires_thumbnail_image) (void	*surface,
				 int    *width,
				 int    *height);

    cairo_warn cairo_int_status_t
    (*set_thumbnail_image) (void	          *surface,
			    cairo_image_surface_t *image);
};

cairo_private cairo_surface_t *
_cairo_paginated_surface_create (cairo_surface_t				*target,
				 cairo_content_t				 content,
				 const cairo_paginated_surface_backend_t	*backend);

cairo_private cairo_surface_t *
_cairo_paginated_surface_get_target (cairo_surface_t *surface);

cairo_private cairo_surface_t *
_cairo_paginated_surface_get_recording (cairo_surface_t *surface);

cairo_private cairo_bool_t
_cairo_surface_is_paginated (cairo_surface_t *surface);

cairo_private cairo_status_t
_cairo_paginated_surface_set_size (cairo_surface_t 	*surface,
				   double		 width,
				   double		 height);

#endif /* CAIRO_PAGINATED_H */
