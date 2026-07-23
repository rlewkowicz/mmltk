/*
 * Copyright © 2018  Google, Inc.
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
 *
 * Google Author(s): Behdad Esfahbod
 */

#include "hb.hh"

#ifndef HB_NO_NAME

#include "hb-ot-name-table.hh"

#include "hb-utf.hh"




const hb_ot_name_entry_t *
hb_ot_name_list_names (hb_face_t    *face,
		       unsigned int *num_entries )
{
  const OT::name_accelerator_t &name = *face->table.name;
  if (num_entries) *num_entries = name.names.length;
  return (const hb_ot_name_entry_t *) name.names;
}

template <typename utf_t>
static inline unsigned int
hb_ot_name_get_utf (hb_face_t       *face,
		    hb_ot_name_id_t  name_id,
		    hb_language_t    language,
		    unsigned int    *text_size ,
		    typename utf_t::codepoint_t *text )
{
  const OT::name_accelerator_t &name = *face->table.name;

  if (!language)
    language = hb_language_from_string ("en", 2);

  unsigned int width;
  int idx = name.get_index (name_id, language, &width);
  if (idx != -1)
  {
    hb_bytes_t bytes = name.get_name (idx);

    if (width == 2) 
      return OT::hb_ot_name_convert_utf<hb_utf16_be_t, utf_t> (bytes, text_size, text);

    if (width == 1) 
      return OT::hb_ot_name_convert_utf<hb_ascii_t, utf_t> (bytes, text_size, text);
  }

  if (text_size)
  {
    if (*text_size)
      *text = 0;
    *text_size = 0;
  }
  return 0;
}

unsigned int
hb_ot_name_get_utf8 (hb_face_t       *face,
		     hb_ot_name_id_t  name_id,
		     hb_language_t    language,
		     unsigned int    *text_size ,
		     char            *text      )
{
  return hb_ot_name_get_utf<hb_utf8_t> (face, name_id, language, text_size,
					(hb_utf8_t::codepoint_t *) text);
}

unsigned int
hb_ot_name_get_utf16 (hb_face_t       *face,
		      hb_ot_name_id_t  name_id,
		      hb_language_t    language,
		      unsigned int    *text_size ,
		      uint16_t        *text      )
{
  return hb_ot_name_get_utf<hb_utf16_t> (face, name_id, language, text_size, text);
}

unsigned int
hb_ot_name_get_utf32 (hb_face_t       *face,
		      hb_ot_name_id_t  name_id,
		      hb_language_t    language,
		      unsigned int    *text_size ,
		      uint32_t        *text      )
{
  return hb_ot_name_get_utf<hb_utf32_t> (face, name_id, language, text_size, text);
}

#endif
