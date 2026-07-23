/*
 * Copyright © 2019  Ebrahim Byagowi
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#if !defined(HB_H_IN) && !defined(HB_NO_SINGLE_HEADER_ERROR)
#error "Include <hb.h> instead."
#endif

#ifndef HB_STYLE_H
#define HB_STYLE_H

#include "hb.h"

HB_BEGIN_DECLS

typedef enum
{
  HB_STYLE_TAG_ITALIC		= HB_TAG ('i','t','a','l'),
  HB_STYLE_TAG_OPTICAL_SIZE	= HB_TAG ('o','p','s','z'),
  HB_STYLE_TAG_SLANT_ANGLE	= HB_TAG ('s','l','n','t'),
  HB_STYLE_TAG_SLANT_RATIO	= HB_TAG ('S','l','n','t'),
  HB_STYLE_TAG_WIDTH		= HB_TAG ('w','d','t','h'),
  HB_STYLE_TAG_WEIGHT		= HB_TAG ('w','g','h','t'),

  _HB_STYLE_TAG_MAX_VALUE	= HB_TAG_MAX_SIGNED 
} hb_style_tag_t;


HB_EXTERN float
hb_style_get_value (hb_font_t *font, hb_style_tag_t style_tag);

HB_END_DECLS

#endif /* HB_STYLE_H */
