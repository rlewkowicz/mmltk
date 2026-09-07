/*
 * Copyright © 2011,2012  Google, Inc.
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

#ifndef HB_OT_HHEA_TABLE_HH
#define HB_OT_HHEA_TABLE_HH

#include "hb-open-type.hh"

#define HB_OT_TAG_hhea HB_TAG('h','h','e','a')
#define HB_OT_TAG_vhea HB_TAG('v','h','e','a')


namespace OT {


template <typename T>
struct _hea
{
  bool has_data () const { return version.major; }

  bool sanitize (hb_sanitize_context_t *c) const
  {
    TRACE_SANITIZE (this);
    return_trace (c->check_struct (this) &&
		  hb_barrier () &&
		  likely (version.major == 1));
  }

  public:
  FixedVersion<>version;	
  FWORD		ascender;	
  FWORD		descender;	
  FWORD		lineGap;	
  UFWORD	advanceMax;	
  FWORD		minLeadingBearing;
  FWORD		minTrailingBearing;
  FWORD		maxExtent;	
  HBINT16	caretSlopeRise;	
  HBINT16	caretSlopeRun;	
  HBINT16	caretOffset;	
  HBINT16	reserved1;	
  HBINT16	reserved2;	
  HBINT16	reserved3;	
  HBINT16	reserved4;	
  HBINT16	metricDataFormat;
  HBUINT16	numberOfLongMetrics;
  public:
  DEFINE_SIZE_STATIC (36);
};

struct hhea : _hea<hhea> {
  static constexpr hb_tag_t tableTag = HB_OT_TAG_hhea;
};
struct vhea : _hea<vhea> {
  static constexpr hb_tag_t tableTag = HB_OT_TAG_vhea;
};


} 


#endif /* HB_OT_HHEA_TABLE_HH */
