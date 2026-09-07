/*
 * Copyright (c) 2014 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL INTEL AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


#ifndef VA_DEC_VP9_H
#define VA_DEC_VP9_H

#ifdef __cplusplus
extern "C" {
#endif





typedef struct  _VADecPictureParameterBufferVP9 {
    uint16_t                frame_width;
    uint16_t                frame_height;

    VASurfaceID             reference_frames[8];

    union {
        struct {
            uint32_t        subsampling_x                               : 1;
            uint32_t        subsampling_y                               : 1;
            uint32_t        frame_type                                  : 1;
            uint32_t        show_frame                                  : 1;
            uint32_t        error_resilient_mode                        : 1;
            uint32_t        intra_only                                  : 1;
            uint32_t        allow_high_precision_mv                     : 1;
            uint32_t        mcomp_filter_type                           : 3;
            uint32_t        frame_parallel_decoding_mode                : 1;
            uint32_t        reset_frame_context                         : 2;
            uint32_t        refresh_frame_context                       : 1;
            uint32_t        frame_context_idx                           : 2;
            uint32_t        segmentation_enabled                        : 1;

            uint32_t        segmentation_temporal_update                : 1;
            uint32_t        segmentation_update_map                     : 1;

            uint32_t        last_ref_frame                              : 3;
            uint32_t        last_ref_frame_sign_bias                    : 1;
            uint32_t        golden_ref_frame                            : 3;
            uint32_t        golden_ref_frame_sign_bias                  : 1;
            uint32_t        alt_ref_frame                               : 3;
            uint32_t        alt_ref_frame_sign_bias                     : 1;
            uint32_t        lossless_flag                               : 1;
        } bits;
        uint32_t            value;
    } pic_fields;

    uint8_t                 filter_level;
    uint8_t                 sharpness_level;

    uint8_t                 log2_tile_rows;
    uint8_t                 log2_tile_columns;
    uint8_t                 frame_header_length_in_bytes;

    uint16_t                first_partition_size;

    uint8_t                 mb_segment_tree_probs[7];
    uint8_t                 segment_pred_probs[3];

    uint8_t                 profile;

    uint8_t                 bit_depth;

    uint32_t                va_reserved[VA_PADDING_MEDIUM];

} VADecPictureParameterBufferVP9;



typedef struct  _VASegmentParameterVP9 {
    union {
        struct {
            uint16_t        segment_reference_enabled                   : 1;
            uint16_t        segment_reference                           : 2;
            uint16_t        segment_reference_skipped                   : 1;
        } fields;
        uint16_t            value;
    } segment_flags;

    uint8_t                 filter_level[4][2];
    int16_t                 luma_ac_quant_scale;
    int16_t                 luma_dc_quant_scale;
    int16_t                 chroma_ac_quant_scale;
    int16_t                 chroma_dc_quant_scale;

    uint32_t                va_reserved[VA_PADDING_LOW];

} VASegmentParameterVP9;



typedef struct _VASliceParameterBufferVP9 {
    uint32_t slice_data_size;
    uint32_t slice_data_offset;
    uint32_t slice_data_flag;

    VASegmentParameterVP9   seg_param[8];

    uint32_t                va_reserved[VA_PADDING_LOW];

} VASliceParameterBufferVP9;



#ifdef __cplusplus
}
#endif

#endif /* VA_DEC_VP9_H */
