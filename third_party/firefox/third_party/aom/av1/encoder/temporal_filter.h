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

#if !defined(AOM_AV1_ENCODER_TEMPORAL_FILTER_H_)
#define AOM_AV1_ENCODER_TEMPORAL_FILTER_H_

#include <stdbool.h>

#include "aom_util/aom_pthread.h"

#if defined(__cplusplus)
extern "C" {
#endif
/*!\cond */
struct AV1_COMP;
struct AV1EncoderConfig;
struct ThreadData;
#define BH 64
#define BW 64

#define TF_BLOCK_SIZE BLOCK_64X64

#define TF_WINDOW_LENGTH 5

#define NUM_16X16 16

static const double SQRT_PI_BY_2 = 1.25331413732;

#define TF_WEIGHT_SCALE 1000
#define TF_WINDOW_BLOCK_BALANCE_WEIGHT 5
#define TF_Q_DECAY_THRESHOLD 20
#define TF_SEARCH_ERROR_NORM_WEIGHT 20
#define TF_STRENGTH_THRESHOLD 4
#define TF_SEARCH_DISTANCE_THRESHOLD 0.1
#define TF_QINDEX_CUTOFF 128

#define NOISE_ESTIMATION_EDGE_THRESHOLD 50

typedef struct {
  int64_t sum;
  int64_t sse;
} FRAME_DIFF;

/*!\endcond */

/*!
 * \brief Parameters related to temporal filtering.
 */
typedef struct {
  /*!
   * Frame buffers used for temporal filtering.
   */
  YV12_BUFFER_CONFIG *frames[MAX_LAG_BUFFERS];
  /*!
   * Number of frames in the frame buffer.
   */
  int num_frames;

  /*!
   * Output filtered frame
   */
  YV12_BUFFER_CONFIG *output_frame;

  /*!
   * Index of the frame to be filtered.
   */
  int filter_frame_idx;
  /*!
   * Whether to accumulate diff for show existing condition check.
   */
  int compute_frame_diff;
  /*!
   * Frame scaling factor.
   */
  struct scale_factors sf;
  /*!
   * Estimated noise levels for each plane in the frame.
   */
  double noise_levels[MAX_MB_PLANE];
  /*!
   * Number of pixels in the temporal filtering block across all planes.
   */
  int num_pels;
  /*!
   * Number of temporal filtering block rows.
   */
  int mb_rows;
  /*!
   * Number of temporal filtering block columns.
   */
  int mb_cols;
  /*!
   * Whether the frame is high-bitdepth or not.
   */
  int is_highbitdepth;
  /*!
   * Quantization factor used in temporal filtering.
   */
  int q_factor;
} TemporalFilterCtx;

/*!
 * buffer count in TEMPORAL_FILTER_INFO
 * Currently we only apply filtering on KEY and ARF after
 * define_gf_group(). Hence, the count is two.
 */
#define TF_INFO_BUF_COUNT 2

/*!
 * \brief Temporal filter info for a gop
 */
typedef struct TEMPORAL_FILTER_INFO {
  /*!
   * A flag indicate whether temporal filter shoud be applied.
   * This flag will stored the result of
   * av1_is_temporal_filter_on()
   */
  int is_temporal_filter_on;
  /*!
   * buffers used for temporal filtering in a GOP
   * index 0 for key frame and index 1 for ARF
   */
  YV12_BUFFER_CONFIG tf_buf[TF_INFO_BUF_COUNT];

  /*!
   * buffers used for temporal filtering for
   * INTNL_ARF_UPDATE
   * Check av1_gop_is_second_arf() for the
   * definition of second_arf in detail
   */
  YV12_BUFFER_CONFIG tf_buf_second_arf;
  /*!
   * whether to show the buffer directly or not.
   */
  FRAME_DIFF frame_diff[TF_INFO_BUF_COUNT];
  /*!
   * the corresponding gf_index for the buffer.
   */
  int tf_buf_gf_index[TF_INFO_BUF_COUNT];
  /*!
   * the display_index offset between next show frame and the frames in the GOP
   */
  int tf_buf_display_index_offset[TF_INFO_BUF_COUNT];
  /*!
   * whether the buf is valid or not.
   */
  int tf_buf_valid[TF_INFO_BUF_COUNT];
} TEMPORAL_FILTER_INFO;

/*!\brief Check whether we should apply temporal filter at all.
 * \param[in]   oxcf           AV1 encoder config
 *
 * \return 1: temporal filter is on 0: temporal is off
 */
int av1_is_temporal_filter_on(const struct AV1EncoderConfig *oxcf);

/*!\brief Allocate buffers for TEMPORAL_FILTER_INFO
 * \param[in,out]   tf_info           Temporal filter info for a gop
 * \param[in,out]   cpi               Top level encoder instance structure
 *
 * \return True on success, false on memory allocation failure.
 */
bool av1_tf_info_alloc(TEMPORAL_FILTER_INFO *tf_info,
                       const struct AV1_COMP *cpi);

/*!\brief Free buffers for TEMPORAL_FILTER_INFO
 * \param[in,out]   tf_info           Temporal filter info for a gop
 */
void av1_tf_info_free(TEMPORAL_FILTER_INFO *tf_info);

/*!\brief Reset validity of tf_buf in TEMPORAL_FILTER_INFO
 * \param[in,out]   tf_info           Temporal filter info for a gop
 */
void av1_tf_info_reset(TEMPORAL_FILTER_INFO *tf_info);

/*!\brief Apply temporal filter for key frame and ARF in a gop
 * \param[in,out]   tf_info           Temporal filter info for a gop
 * \param[in,out]   cpi               Top level encoder instance structure
 * \param[in]       gf_group          GF/ARF group data structure
 */
void av1_tf_info_filtering(TEMPORAL_FILTER_INFO *tf_info, struct AV1_COMP *cpi,
                           const GF_GROUP *gf_group);

/*!\brief Get a filtered buffer from TEMPORAL_FILTER_INFO
 * \param[in,out]   tf_info           Temporal filter info for a gop
 * \param[in]       gf_index          gf_index for the target buffer
 * \param[out]      show_tf_buf       whether the target buffer can be shown
 * directly
 */
YV12_BUFFER_CONFIG *av1_tf_info_get_filtered_buf(TEMPORAL_FILTER_INFO *tf_info,
                                                 int gf_index,
                                                 FRAME_DIFF *frame_diff);

/*!\cond */

typedef struct {
  FRAME_DIFF diff;
  MB_MODE_INFO *tmp_mbmi;
  uint32_t *accum;
  uint16_t *count;
  uint8_t *pred;
} TemporalFilterData;

typedef struct {
#if CONFIG_MULTITHREAD
  pthread_mutex_t *mutex_;
#endif
  int next_tf_row;
  bool tf_mt_exit;
} AV1TemporalFilterSync;

void av1_estimate_noise_level(const YV12_BUFFER_CONFIG *frame,
                              double *noise_level, int plane_from, int plane_to,
                              int bit_depth, int edge_thresh);
/*!\endcond */

/*!\brief Does temporal filter for a given macroblock row.
*
* \ingroup src_frame_proc
* \param[in]   cpi                   Top level encoder instance structure
* \param[in]   td                    Pointer to thread data
* \param[in]   mb_row                Macroblock row to be filtered
filtering
*
* \remark Nothing will be returned, but the contents of td->diff will be
modified.
*/
void av1_tf_do_filtering_row(struct AV1_COMP *cpi, struct ThreadData *td,
                             int mb_row);

/*!\brief Performs temporal filtering if needed on a source frame.
 * For example to create a filtered alternate reference frame (ARF)
 *
 * In this function, the lookahead index is different from the 0-based
 * real index. For example, if we want to filter the first frame in the
 * pre-fetched buffer `cpi->lookahead`, the lookahead index will be -1 instead
 * of 0. More concretely, 0 indicates the first LOOKAHEAD frame, which is the
 * second frame in the pre-fetched buffer. Another example: if we want to filter
 * the 17-th frame, which is an ARF, the lookahead index is 15 instead of 16.
 * Futhermore, negative number is used for key frame in one-pass mode, where key
 * frame is filtered with the frames before it instead of after it. For example,
 * -15 means to filter the 17-th frame, which is a key frame in one-pass mode.
 *
 * \ingroup src_frame_proc
 * \param[in]      cpi                        Top level encoder instance
 *                                            structure
 * \param[in]      filter_frame_lookahead_idx The index of the
 *                                            to-filter frame in the lookahead
 *                                            buffer cpi->lookahead.
 * \param[in]      gf_frame_index             Index of GOP
 * \param[in,out]  frame_diff                 structure of sse and sum of the
 *                                            filtered frame.
 * \param[out]     output_frame               Ouput filtered frame.
 */
void av1_temporal_filter(struct AV1_COMP *cpi,
                         const int filter_frame_lookahead_idx,
                         int gf_frame_index, FRAME_DIFF *frame_diff,
                         YV12_BUFFER_CONFIG *output_frame);

/*!\brief Check whether a filtered frame can be show directly
 *
 * This function will use the filtered frame's sse and current q index
 * to make decision.
 *
 * \ingroup src_frame_proc
 * \param[in]  frame          filtered frame's buffer
 * \param[in]  frame_diff     structure of sse and sum of the
 *                            filtered frame.
 * \param[in]  q_index        q_index used for this frame
 * \param[in]  bit_depth      bit depth
 * \param[in]  enable_overlay arf overlay is enabled or disabled
 * \param[in]  is_second_arf  whether or not this is a second ARF frame
 * \return     return 1 if this frame can be shown directly, otherwise
 *             return 0
 */
int av1_check_show_filtered_frame(const YV12_BUFFER_CONFIG *frame,
                                  const FRAME_DIFF *frame_diff, int q_index,
                                  aom_bit_depth_t bit_depth, int enable_overlay,
                                  int is_second_arf);

/*!\cond */
static inline bool tf_alloc_and_reset_data(TemporalFilterData *tf_data,
                                           int num_pels, int is_high_bitdepth) {
  tf_data->tmp_mbmi = (MB_MODE_INFO *)aom_calloc(1, sizeof(*tf_data->tmp_mbmi));
  tf_data->accum =
      (uint32_t *)aom_memalign(16, num_pels * sizeof(*tf_data->accum));
  tf_data->count =
      (uint16_t *)aom_memalign(16, num_pels * sizeof(*tf_data->count));
  if (is_high_bitdepth)
    tf_data->pred = CONVERT_TO_BYTEPTR(
        aom_memalign(32, num_pels * 2 * sizeof(*tf_data->pred)));
  else
    tf_data->pred =
        (uint8_t *)aom_memalign(32, num_pels * sizeof(*tf_data->pred));
  if (!(tf_data->tmp_mbmi && tf_data->accum && tf_data->count && tf_data->pred))
    return false;
  memset(&tf_data->diff, 0, sizeof(tf_data->diff));
  return true;
}

static inline void tf_setup_macroblockd(MACROBLOCKD *mbd,
                                        TemporalFilterData *tf_data,
                                        const struct scale_factors *scale) {
  mbd->block_ref_scale_factors[0] = scale;
  mbd->block_ref_scale_factors[1] = scale;
  mbd->mi = &tf_data->tmp_mbmi;
  mbd->mi[0]->motion_mode = SIMPLE_TRANSLATION;
}

static inline void tf_dealloc_data(TemporalFilterData *tf_data,
                                   int is_high_bitdepth) {
  if (is_high_bitdepth)
    tf_data->pred = (uint8_t *)CONVERT_TO_SHORTPTR(tf_data->pred);
  aom_free(tf_data->tmp_mbmi);
  tf_data->tmp_mbmi = NULL;
  aom_free(tf_data->accum);
  tf_data->accum = NULL;
  aom_free(tf_data->count);
  tf_data->count = NULL;
  aom_free(tf_data->pred);
  tf_data->pred = NULL;
}

static inline void tf_save_state(MACROBLOCKD *mbd, MB_MODE_INFO ***input_mbmi,
                                 uint8_t **input_buffer, int num_planes) {
  for (int i = 0; i < num_planes; i++) {
    input_buffer[i] = mbd->plane[i].pre[0].buf;
  }
  *input_mbmi = mbd->mi;
}

static inline void tf_restore_state(MACROBLOCKD *mbd, MB_MODE_INFO **input_mbmi,
                                    uint8_t **input_buffer, int num_planes) {
  for (int i = 0; i < num_planes; i++) {
    mbd->plane[i].pre[0].buf = input_buffer[i];
  }
  mbd->mi = input_mbmi;
}

/*!\endcond */
#if defined(__cplusplus)
}  
#endif

#endif
