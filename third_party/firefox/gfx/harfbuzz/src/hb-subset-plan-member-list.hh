/*
 * Copyright © 2018  Google, Inc.
 * Copyright © 2023  Behdad Esfahbod
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
 * Google Author(s): Garret Rieger, Roderick Sheeter
 */

#ifndef HB_SUBSET_PLAN_MEMBER_LIST_HH
#define HB_SUBSET_PLAN_MEMBER_LIST_HH
#endif /* HB_SUBSET_PLAN_MEMBER_LIST_HH */ /* Dummy header guards */

#define E(x, y) x, y

HB_SUBSET_PLAN_MEMBER (hb_set_t, unicodes)
HB_SUBSET_PLAN_MEMBER (hb_sorted_vector_t<hb_codepoint_pair_t>, unicode_to_new_gid_list)

HB_SUBSET_PLAN_MEMBER (hb_sorted_vector_t<hb_codepoint_pair_t>, new_to_old_gid_list)

HB_SUBSET_PLAN_MEMBER (hb_set_t, name_ids)

HB_SUBSET_PLAN_MEMBER (hb_set_t, name_languages)

HB_SUBSET_PLAN_MEMBER (hb_set_t, layout_features)

HB_SUBSET_PLAN_MEMBER (hb_set_t, layout_scripts)

HB_SUBSET_PLAN_MEMBER (hb_set_t, glyphs_requested)

HB_SUBSET_PLAN_MEMBER (hb_set_t, no_subset_tables)

HB_SUBSET_PLAN_MEMBER (hb_set_t, drop_tables)

HB_SUBSET_PLAN_MEMBER (hb_map_t, glyph_map_gsub)

HB_SUBSET_PLAN_MEMBER (hb_set_t, _glyphset)
HB_SUBSET_PLAN_MEMBER (hb_set_t, _glyphset_gsub)
HB_SUBSET_PLAN_MEMBER (hb_set_t, _glyphset_mathed)
HB_SUBSET_PLAN_MEMBER (hb_set_t, _glyphset_colred)

HB_SUBSET_PLAN_MEMBER (hb_map_t, gsub_lookups)
HB_SUBSET_PLAN_MEMBER (hb_map_t, gpos_lookups)

HB_SUBSET_PLAN_MEMBER (hb_map_t, used_mark_sets_map)

HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<unsigned, hb::unique_ptr<hb_set_t>>), gsub_langsys)
HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<unsigned, hb::unique_ptr<hb_set_t>>), gpos_langsys)

HB_SUBSET_PLAN_MEMBER (hb_map_t, gsub_features)
HB_SUBSET_PLAN_MEMBER (hb_map_t, gpos_features)

HB_SUBSET_PLAN_MEMBER (hb_map_t, gsub_features_w_duplicates)
HB_SUBSET_PLAN_MEMBER (hb_map_t, gpos_features_w_duplicates)

HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<unsigned, hb::shared_ptr<hb_set_t>>), gsub_feature_record_cond_idx_map)
HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<unsigned, hb::shared_ptr<hb_set_t>>), gpos_feature_record_cond_idx_map)

HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<unsigned, const OT::Feature*>), gsub_feature_substitutes_map)
HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<unsigned, const OT::Feature*>), gpos_feature_substitutes_map)

HB_SUBSET_PLAN_MEMBER (hb_set_t, gsub_old_features)
HB_SUBSET_PLAN_MEMBER (hb_set_t, gpos_old_features)

HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<unsigned, hb_pair_t E(<const void*, const void*>)>), gsub_old_feature_idx_tag_map)
HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<unsigned, hb_pair_t E(<const void*, const void*>)>), gpos_old_feature_idx_tag_map)

HB_SUBSET_PLAN_MEMBER (hb_map_t, colrv1_layers)
HB_SUBSET_PLAN_MEMBER (hb_map_t, colr_palettes)
HB_SUBSET_PLAN_MEMBER (hb_vector_t<hb_inc_bimap_t>, colrv1_varstore_inner_maps)
HB_SUBSET_PLAN_MEMBER (mutable hb_hashmap_t E(<unsigned, hb_pair_t E(<unsigned, int>)>), colrv1_variation_idx_delta_map)
HB_SUBSET_PLAN_MEMBER (hb_map_t, colrv1_new_deltaset_idx_varidx_map)

HB_SUBSET_PLAN_MEMBER (mutable hb_hashmap_t E(<unsigned, hb_pair_t E(<unsigned, int>)>), layout_variation_idx_delta_map)

HB_SUBSET_PLAN_MEMBER (hb_vector_t<hb_inc_bimap_t>, gdef_varstore_inner_maps)

HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<hb_tag_t, hb::unique_ptr<hb_blob_t>>), sanitized_table_cache)

HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<hb_tag_t, Triple>), axes_location)
HB_SUBSET_PLAN_MEMBER (hb_vector_t<int>, normalized_coords)

HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<hb_tag_t, Triple>), user_axes_location)
HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<hb_tag_t, TripleDistances>), axes_triple_distances)

HB_SUBSET_PLAN_MEMBER (hb_map_t, axes_index_map)

HB_SUBSET_PLAN_MEMBER (hb_map_t, axes_old_index_tag_map)
HB_SUBSET_PLAN_MEMBER (hb_vector_t<hb_tag_t>, axis_tags)

HB_SUBSET_PLAN_MEMBER (mutable hb_hashmap_t E(<hb_codepoint_t, hb_pair_t E(<unsigned, int>)>), hmtx_map)
HB_SUBSET_PLAN_MEMBER (mutable hb_hashmap_t E(<hb_codepoint_t, hb_pair_t E(<unsigned, int>)>), vmtx_map)
HB_SUBSET_PLAN_MEMBER (mutable hb_vector_t<unsigned>, bounds_width_vec)
HB_SUBSET_PLAN_MEMBER (mutable hb_vector_t<unsigned>, bounds_height_vec)

HB_SUBSET_PLAN_MEMBER (mutable hb_hashmap_t E(<hb_codepoint_t, contour_point_vector_t>), new_gid_contour_points_map)

HB_SUBSET_PLAN_MEMBER (hb_set_t, composite_new_gids)

HB_SUBSET_PLAN_MEMBER (mutable hb_hashmap_t E(<unsigned, hb_pair_t E(<unsigned, int>)>), base_variation_idx_map)

HB_SUBSET_PLAN_MEMBER (hb_vector_t<hb_inc_bimap_t>, base_varstore_inner_maps)

#ifdef HB_EXPERIMENTAL_API
HB_SUBSET_PLAN_MEMBER (hb_hashmap_t E(<hb_ot_name_record_ids_t, hb_bytes_t>), name_table_overrides)
#endif

#undef E
