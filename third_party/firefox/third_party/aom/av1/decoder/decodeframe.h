/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#if !defined(AOM_AV1_DECODER_DECODEFRAME_H_)
#define AOM_AV1_DECODER_DECODEFRAME_H_

#if defined(__cplusplus)
extern "C" {
#endif

struct AV1Decoder;
struct aom_read_bit_buffer;
struct ThreadData;

void av1_read_sequence_header(AV1_COMMON *cm, struct aom_read_bit_buffer *rb,
                              SequenceHeader *seq_params);

BITSTREAM_PROFILE av1_read_profile(struct aom_read_bit_buffer *rb);

int av1_check_trailing_bits(struct AV1Decoder *pbi,
                            struct aom_read_bit_buffer *rb);

uint32_t av1_decode_frame_headers_and_setup(struct AV1Decoder *pbi,
                                            struct aom_read_bit_buffer *rb,
                                            int trailing_bits_present);

void av1_decode_tg_tiles_and_wrapup(struct AV1Decoder *pbi, const uint8_t *data,
                                    const uint8_t *data_end,
                                    const uint8_t **p_data_end, int start_tile,
                                    int end_tile, int initialize_flag);

void av1_read_color_config(struct aom_read_bit_buffer *rb,
                           int allow_lowbitdepth, SequenceHeader *seq_params,
                           struct aom_internal_error_info *error_info);

void av1_read_timing_info_header(aom_timing_info_t *timing_info,
                                 struct aom_internal_error_info *error,
                                 struct aom_read_bit_buffer *rb);

void av1_read_decoder_model_info(aom_dec_model_info_t *decoder_model_info,
                                 struct aom_read_bit_buffer *rb);

void av1_read_op_parameters_info(aom_dec_model_op_parameters_t *op_params,
                                 int buffer_delay_length,
                                 struct aom_read_bit_buffer *rb);

struct aom_read_bit_buffer *av1_init_read_bit_buffer(
    struct AV1Decoder *pbi, struct aom_read_bit_buffer *rb, const uint8_t *data,
    const uint8_t *data_end);

void av1_free_mc_tmp_buf(struct ThreadData *thread_data);

void av1_set_single_tile_decoding_mode(AV1_COMMON *const cm);

#if defined(__cplusplus)
}  
#endif

#endif
