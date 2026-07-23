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

/*! \file
 * Declares various structs used to encode the current partition block.
 */
#if !defined(AOM_AV1_ENCODER_BLOCK_H_)
#define AOM_AV1_ENCODER_BLOCK_H_

#include "av1/common/blockd.h"
#include "av1/common/entropymv.h"
#include "av1/common/entropy.h"
#include "av1/common/enums.h"
#include "av1/common/mvref_common.h"

#include "av1/encoder/enc_enums.h"
#include "av1/encoder/mcomp_structs.h"
#if !CONFIG_REALTIME_ONLY
#include "av1/encoder/partition_cnn_weights.h"
#endif

#include "av1/encoder/hash_motion.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define MIN_TPL_BSIZE_1D 16
#define MAX_TPL_BLK_IN_SB (MAX_SB_SIZE / MIN_TPL_BSIZE_1D)
#define RD_RECORD_BUFFER_LEN 8

/*! Maximum value taken by transform type probabilities */
#define MAX_TX_TYPE_PROB 1024

/*! Maximum value of inter transform RD records. */
#define TOP_INTER_TX_NO_SPLIT_COUNT 4

#define COLOR_SENS_IDX(plane) ((plane) - 1)

#define COLLECT_NONRD_PICK_MODE_STAT 0

/*!\cond */
#if COLLECT_NONRD_PICK_MODE_STAT
#include "aom_ports/aom_timer.h"

typedef struct _mode_search_stat_nonrd {
  int32_t num_blocks[BLOCK_SIZES];
  int64_t total_block_times[BLOCK_SIZES];
  int32_t num_searches[BLOCK_SIZES][MB_MODE_COUNT];
  int32_t num_nonskipped_searches[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t search_times[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t nonskipped_search_times[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t ms_time[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t ifs_time[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t model_rd_time[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t txfm_time[BLOCK_SIZES][MB_MODE_COUNT];
  struct aom_usec_timer timer1;
  struct aom_usec_timer timer2;
  struct aom_usec_timer bsize_timer;
} mode_search_stat_nonrd;
#endif
/*!\endcond */

/*! \brief Superblock level encoder info
 *
 * SuperblockEnc stores superblock level information used by the encoder for
 * more efficient encoding. Currently this is mostly used to store TPL data
 * for the current superblock.
 */
typedef struct {
  BLOCK_SIZE min_partition_size;
  BLOCK_SIZE max_partition_size;

  int tpl_data_count;
  int64_t tpl_inter_cost[MAX_TPL_BLK_IN_SB * MAX_TPL_BLK_IN_SB];
  int64_t tpl_intra_cost[MAX_TPL_BLK_IN_SB * MAX_TPL_BLK_IN_SB];
  int_mv tpl_mv[MAX_TPL_BLK_IN_SB * MAX_TPL_BLK_IN_SB][INTER_REFS_PER_FRAME];
  int tpl_stride;
} SuperBlockEnc;

/*! \brief Stores the best performing modes.
 */
typedef struct {
  MB_MODE_INFO mbmi;
  RD_STATS rd_cost;
  int64_t rd;
  int rate_y;
  int rate_uv;
  uint8_t color_index_map[MAX_SB_SQUARE];
  THR_MODES mode_index;
} WinnerModeStats;

/*! \brief Each source plane of the current macroblock
 *
 * This struct also stores the txfm buffers and quantizer settings.
 */
typedef struct macroblock_plane {
  int16_t *src_diff;
  tran_low_t *dqcoeff;
  tran_low_t *qcoeff;
  tran_low_t *coeff;
  uint16_t *eobs;
  uint8_t *txb_entropy_ctx;
  struct buf_2d src;

  /*! \name Quantizer Settings
   *
   * \attention These are used/accessed only in the quantization process.
   * RDO does not and *must not* depend on any of these values.
   * All values below share the coefficient scale/shift used in TX.
   */
  const int16_t *quant_fp_QTX;
  const int16_t *round_fp_QTX;
  const int16_t *quant_QTX;
  const int16_t *round_QTX;
  const int16_t *quant_shift_QTX;
  const int16_t *zbin_QTX;
  const int16_t *dequant_QTX;
} MACROBLOCK_PLANE;

/*! \brief Costs for encoding the coefficients within a level.
 *
 * Covers everything including txb_skip, eob, dc_sign,
 */
typedef struct {
  int txb_skip_cost[TXB_SKIP_CONTEXTS][2];
  /*! \brief Cost for encoding the base_eob of a level.
   *
   * Decoder uses base_eob to derive the base_level as base_eob := base_eob+1.
   */
  int base_eob_cost[SIG_COEF_CONTEXTS_EOB][3];
  /*! \brief Cost for encoding the base level of a coefficient.
   *
   * Decoder derives coeff_base as coeff_base := base_eob + 1.
   */
  int base_cost[SIG_COEF_CONTEXTS][8];
  /*! \brief Cost for encoding the last non-zero coefficient.
   *
   * Eob is derived from eob_extra at the decoder as eob := eob_extra + 1
   */
  int eob_extra_cost[EOB_COEF_CONTEXTS][2];
  int dc_sign_cost[DC_SIGN_CONTEXTS][2];
  int lps_cost[LEVEL_CONTEXTS][COEFF_BASE_RANGE + 1 + COEFF_BASE_RANGE + 1];
} LV_MAP_COEFF_COST;

/*! \brief Costs for encoding the eob.
 */
typedef struct {
  int eob_cost[2][11];
} LV_MAP_EOB_COST;

/*! \brief Stores the transforms coefficients for the whole superblock.
 */
typedef struct {
  tran_low_t *tcoeff[MAX_MB_PLANE];
  uint16_t *eobs[MAX_MB_PLANE];
  /*! \brief Transform block entropy contexts.
   *
   * Each element is used as a bit field.
   * - Bits 0~3: txb_skip_ctx
   * - Bits 4~5: dc_sign_ctx.
   */
  uint8_t *entropy_ctx[MAX_MB_PLANE];
} CB_COEFF_BUFFER;

/*! \brief Extended mode info derived from mbmi.
 */
typedef struct {
  CANDIDATE_MV ref_mv_stack[MODE_CTX_REF_FRAMES][USABLE_REF_MV_STACK_SIZE];
  uint16_t weight[MODE_CTX_REF_FRAMES][USABLE_REF_MV_STACK_SIZE];
  uint8_t ref_mv_count[MODE_CTX_REF_FRAMES];
  int_mv global_mvs[REF_FRAMES];
  int16_t mode_context[MODE_CTX_REF_FRAMES];
} MB_MODE_INFO_EXT;

/*! \brief Stores best extended mode information at frame level.
 *
 * The frame level in here is used in bitstream preparation stage. The
 * information in \ref MB_MODE_INFO_EXT are copied to this struct to save
 * memory.
 */
typedef struct {
  CANDIDATE_MV ref_mv_stack[USABLE_REF_MV_STACK_SIZE];
  uint16_t weight[USABLE_REF_MV_STACK_SIZE];
  uint8_t ref_mv_count;
  int_mv global_mvs[REF_FRAMES];
  int16_t mode_context;
  uint16_t cb_offset[PLANE_TYPES];
} MB_MODE_INFO_EXT_FRAME;

/*! \brief Inter-mode txfm results for a partition block.
 */
typedef struct {
  TX_SIZE tx_size;
  TX_SIZE inter_tx_size[INTER_TX_SIZE_BUF_LEN];
  uint8_t blk_skip[MAX_MIB_SIZE * MAX_MIB_SIZE];
  uint8_t tx_type_map[MAX_MIB_SIZE * MAX_MIB_SIZE];
  RD_STATS rd_stats;
  uint32_t hash_value;
} MB_RD_INFO;

/*! \brief Hash records of the inter-mode transform results
 *
 * Hash records of the inter-mode transform results for a whole partition block
 * based on the residue. Since this operates on the partition block level, this
 * can give us a whole txfm partition tree.
 */
typedef struct {
  /*! Circular buffer that stores the inter-mode txfm results of a partition
   *  block.
   */
  MB_RD_INFO mb_rd_info[RD_RECORD_BUFFER_LEN];
  int index_start;
  int num;
  CRC32C crc_calculator;
} MB_RD_RECORD;

#define MAX_COMP_RD_STATS 64
/*! \brief Rdcost stats in compound mode.
 */
typedef struct {
  int32_t rate[COMPOUND_TYPES];
  int64_t dist[COMPOUND_TYPES];
  int32_t model_rate[COMPOUND_TYPES];
  int64_t model_dist[COMPOUND_TYPES];
  int comp_rs2[COMPOUND_TYPES];
  int_mv mv[2];
  MV_REFERENCE_FRAME ref_frames[2];
  PREDICTION_MODE mode;
  int_interpfilters filter;
  int ref_mv_idx;
  int is_global[2];
  INTERINTER_COMPOUND_DATA interinter_comp;
} COMP_RD_STATS;

/*! \brief Contains buffers used to speed up rdopt for obmc.
 *
 * See the comments for calc_target_weighted_pred for details.
 */
typedef struct {
  /*! \brief A new source weighted with the above and left predictors.
   *
   * Used to efficiently construct multiple obmc predictors during rdopt.
   */
  int32_t *wsrc;
  /*! \brief A new mask constructed from the original horz/vert mask.
   *
   * \copydetails wsrc
   */
  int32_t *mask;
  /*! \brief Prediction from the up predictor.
   *
   * Used to build the obmc predictor.
   */
  uint8_t *above_pred;
  /*! \brief Prediction from the up predictor.
   *
   * \copydetails above_pred
   */
  uint8_t *left_pred;
} OBMCBuffer;

/*! \brief Contains color maps used in palette mode.
 */
typedef struct {
  uint8_t best_palette_color_map[MAX_PALETTE_SQUARE];
  int16_t kmeans_data_buf[2 * MAX_PALETTE_SQUARE];
} PALETTE_BUFFER;

/*! \brief Contains buffers used by av1_compound_type_rd()
 *
 * For sizes and alignment of these arrays, refer to
 * alloc_compound_type_rd_buffers() function.
 */
typedef struct {
  uint8_t *pred0;
  uint8_t *pred1;
  int16_t *residual1;
  int16_t *diff10;
  uint8_t *tmp_best_mask_buf;
} CompoundTypeRdBuffers;

/*! \brief Holds some parameters related to partitioning schemes in AV1.
 */
typedef struct {
#if !CONFIG_REALTIME_ONLY
  /*! \brief Current index on the partition block quad tree.
   *
   * Used to index into the cnn buffer for partition decision.
   */
  int quad_tree_idx;
  int cnn_output_valid;
  float cnn_buffer[CNN_OUT_BUF_SIZE];
  float log_q;
#endif

  /*! \brief Variance of the subblocks in the superblock.
   *
   * This is used by rt mode for variance based partitioning.
   * The indices corresponds to the following block sizes:
   * -   0    - 128x128
   * -  1-2   - 128x64
   * -  3-4   -  64x128
   * -  5-8   -  64x64
   * -  9-16  -  64x32
   * - 17-24  -  32x64
   * - 25-40  -  32x32
   * - 41-104 -  16x16
   */
  uint8_t variance_low[105];
} PartitionSearchInfo;

/*!\cond */
enum {
  TX_PRUNE_NONE = 0,
  TX_PRUNE_LARGEST = 1,
  TX_PRUNE_SPLIT = 2,
} UENUM1BYTE(TX_PRUNE_TYPE);
/*!\endcond */

/*! \brief Defines the parameters used to perform txfm search.
 *
 * For the most part, this determines how various speed features are used.
 */
typedef struct {
  /*! \brief Whether to limit the intra txfm search type to the default txfm.
   *
   * This could either be a result of either sequence parameter or speed
   * features.
   */
  int use_default_intra_tx_type;

  /*! Whether to limit the intra transform search type to the ones in the table
   * av1_derived_intra_tx_used_flag[INTRA_MODES].
   */
  int use_derived_intra_tx_type_set;

  /*! Probability threshold used for conditionally forcing tx type*/
  int default_inter_tx_type_prob_thresh;

  int prune_2d_txfm_mode;

  /*! \brief Variable from \ref WinnerModeParams based on current eval mode.
   *
   * See the documentation for \ref WinnerModeParams for more detail.
   */
  unsigned int coeff_opt_thresholds[2];
  /*! \copydoc coeff_opt_thresholds */
  unsigned int tx_domain_dist_threshold;
  /*! \copydoc coeff_opt_thresholds */
  TX_SIZE_SEARCH_METHOD tx_size_search_method;
  /*! \copydoc coeff_opt_thresholds */
  unsigned int use_transform_domain_distortion;
  /*! \copydoc coeff_opt_thresholds */
  unsigned int skip_txfm_level;

  /*! \brief How to search for the optimal tx_size
   *
   * If ONLY_4X4, use TX_4X4; if TX_MODE_LARGEST, use the largest tx_size for
   * the current partition block; if TX_MODE_SELECT, search through the whole
   * tree.
   *
   * \attention
   * Although this looks suspicious similar to a bitstream element, this
   * tx_mode_search_type is only used internally by the encoder, and is *not*
   * written to the bitstream. It determines what kind of tx_mode would be
   * searched. For example, we might set it to TX_MODE_LARGEST to find a good
   * candidate, then code it as TX_MODE_SELECT.
   */
  TX_MODE tx_mode_search_type;

  /*!
   * Determines whether a block can be predicted as transform skip or DC only
   * based on residual mean and variance.
   * Type 0 : No skip block or DC only block prediction
   * Type 1 : Prediction of skip block based on residual mean and variance
   * Type 2 : Prediction of skip block or DC only block based on residual mean
   * and variance
   */
  unsigned int predict_dc_level;

  /*!
   * Whether or not we should use the quantization matrix as weights for PSNR
   * during RD search.
   */
  int use_qm_dist_metric;

  /*!
   * Keep track of previous mode evaluation stage type. This will be used to
   * reset mb rd hash record when mode evaluation type changes.
   */
  int mode_eval_type;

#if !CONFIG_REALTIME_ONLY
  TX_PRUNE_TYPE nn_prune_depths_for_intra_tx;

  /*! \brief Indicates if NN model should be invoked to prune transform depths.
   *
   * Used to signal whether NN model should be evaluated to prune the R-D
   * evaluation of specific transform depths.
   */
  bool enable_nn_prune_intra_tx_depths;
#endif
} TxfmSearchParams;

/*!\cond */
#define MAX_NUM_8X8_TXBS ((MAX_MIB_SIZE >> 1) * (MAX_MIB_SIZE >> 1))
#define MAX_NUM_16X16_TXBS ((MAX_MIB_SIZE >> 2) * (MAX_MIB_SIZE >> 2))
#define MAX_NUM_32X32_TXBS ((MAX_MIB_SIZE >> 3) * (MAX_MIB_SIZE >> 3))
#define MAX_NUM_64X64_TXBS ((MAX_MIB_SIZE >> 4) * (MAX_MIB_SIZE >> 4))
/*!\endcond */

/*! \brief Stores various encoding/search decisions related to txfm search.
 *
 * This struct contains a cache of previous txfm results, and some buffers for
 * the current txfm decision.
 */
typedef struct {
  uint8_t skip_txfm;

  /*! \brief Whether to skip transform and quantization on a txfm block level.
   *
   * Skips transform and quantization on a transform block level inside the
   * current partition block. Each element of this array is used as a bit-field.
   * So for example, the we are skipping on the luma plane, then the last bit
   * would be set to 1.
   */
  uint8_t blk_skip[MAX_MIB_SIZE * MAX_MIB_SIZE];

  /*! \brief Transform types inside the partition block
   *
   * Keeps a record of what kind of transform to use for each of the transform
   * block inside the partition block.
   * \attention The buffer here is *never* directly used. Instead, this just
   * allocates the memory for MACROBLOCKD::tx_type_map during rdopt on the
   * partition block. So if we need to save memory, we could move the allocation
   * to pick_sb_mode instead.
   */
  uint8_t tx_type_map_[MAX_MIB_SIZE * MAX_MIB_SIZE];

  MB_RD_RECORD *mb_rd_record;

  /*! \brief Number of txb splits.
   *
   * Keep track of how many times we've used split tx partition for transform
   * blocks. Somewhat misleadingly, this parameter doesn't actually keep track
   * of the count of the current block. Instead, it's a cumulative count across
   * of the whole frame. The main usage is that if txb_split_count is zero, then
   * we can signal TX_MODE_LARGEST at frame level.
   */
  unsigned int txb_split_count;
#if CONFIG_SPEED_STATS
  unsigned int tx_search_count;
#endif
} TxfmSearchInfo;
#undef MAX_NUM_8X8_TXBS
#undef MAX_NUM_16X16_TXBS
#undef MAX_NUM_32X32_TXBS
#undef MAX_NUM_64X64_TXBS

/*! \brief Holds the entropy costs for various modes sent to the bitstream.
 *
 * \attention This does not include the costs for mv and transformed
 * coefficients.
 */
typedef struct {
  int partition_cost[PARTITION_CONTEXTS][EXT_PARTITION_TYPES];

  int mbmode_cost[BLOCK_SIZE_GROUPS][INTRA_MODES];
  int y_mode_costs[INTRA_MODES][INTRA_MODES][INTRA_MODES];
  int intra_uv_mode_cost[CFL_ALLOWED_TYPES][INTRA_MODES][UV_INTRA_MODES];
  int filter_intra_cost[BLOCK_SIZES_ALL][2];
  int filter_intra_mode_cost[FILTER_INTRA_MODES];
  int angle_delta_cost[DIRECTIONAL_MODES][2 * MAX_ANGLE_DELTA + 1];

  int cfl_cost[CFL_JOINT_SIGNS][CFL_PRED_PLANES][CFL_ALPHABET_SIZE];

  int intrabc_cost[2];

  int palette_y_size_cost[PALATTE_BSIZE_CTXS][PALETTE_SIZES];
  int palette_uv_size_cost[PALATTE_BSIZE_CTXS][PALETTE_SIZES];
  int palette_y_color_cost[PALETTE_SIZES][PALETTE_COLOR_INDEX_CONTEXTS]
                          [PALETTE_COLORS];
  int palette_uv_color_cost[PALETTE_SIZES][PALETTE_COLOR_INDEX_CONTEXTS]
                           [PALETTE_COLORS];
  int palette_y_mode_cost[PALATTE_BSIZE_CTXS][PALETTE_Y_MODE_CONTEXTS][2];
  int palette_uv_mode_cost[PALETTE_UV_MODE_CONTEXTS][2];

  int skip_mode_cost[SKIP_MODE_CONTEXTS][2];
  int newmv_mode_cost[NEWMV_MODE_CONTEXTS][2];
  int zeromv_mode_cost[GLOBALMV_MODE_CONTEXTS][2];
  int refmv_mode_cost[REFMV_MODE_CONTEXTS][2];
  int drl_mode_cost0[DRL_MODE_CONTEXTS][2];

  int single_ref_cost[REF_CONTEXTS][SINGLE_REFS - 1][2];
  int comp_inter_cost[COMP_INTER_CONTEXTS][2];
  int comp_ref_type_cost[COMP_REF_TYPE_CONTEXTS]
                        [CDF_SIZE(COMP_REFERENCE_TYPES)];
  int uni_comp_ref_cost[UNI_COMP_REF_CONTEXTS][UNIDIR_COMP_REFS - 1]
                       [CDF_SIZE(2)];
  /*! \brief Cost for signaling ref_frame[0] in bidir-comp mode
   *
   * Includes LAST_FRAME, LAST2_FRAME, LAST3_FRAME, and GOLDEN_FRAME.
   */
  int comp_ref_cost[REF_CONTEXTS][FWD_REFS - 1][2];
  /*! \brief Cost for signaling ref_frame[1] in bidir-comp mode
   *
   * Includes ALTREF_FRAME, ALTREF2_FRAME, and BWDREF_FRAME.
   */
  int comp_bwdref_cost[REF_CONTEXTS][BWD_REFS - 1][2];

  int intra_inter_cost[INTRA_INTER_CONTEXTS][2];
  int inter_compound_mode_cost[INTER_MODE_CONTEXTS][INTER_COMPOUND_MODES];
  int compound_type_cost[BLOCK_SIZES_ALL][MASKED_COMPOUND_TYPES];
  int wedge_idx_cost[BLOCK_SIZES_ALL][16];
  int interintra_cost[BLOCK_SIZE_GROUPS][2];
  int wedge_interintra_cost[BLOCK_SIZES_ALL][2];
  int interintra_mode_cost[BLOCK_SIZE_GROUPS][INTERINTRA_MODES];

  int comp_idx_cost[COMP_INDEX_CONTEXTS][2];
  int comp_group_idx_cost[COMP_GROUP_IDX_CONTEXTS][2];

  int motion_mode_cost[BLOCK_SIZES_ALL][MOTION_MODES];
  int motion_mode_cost1[BLOCK_SIZES_ALL][2];
  int switchable_interp_costs[SWITCHABLE_FILTER_CONTEXTS][SWITCHABLE_FILTERS];

  int skip_txfm_cost[SKIP_CONTEXTS][2];
  int tx_size_cost[TX_SIZES - 1][TX_SIZE_CONTEXTS][TX_SIZES];
  int txfm_partition_cost[TXFM_PARTITION_CONTEXTS][2];
  int inter_tx_type_costs[EXT_TX_SETS_INTER][EXT_TX_SIZES][TX_TYPES];
  int intra_tx_type_costs[EXT_TX_SETS_INTRA][EXT_TX_SIZES][INTRA_MODES]
                         [TX_TYPES];

  int switchable_restore_cost[RESTORE_SWITCHABLE_TYPES];
  int wiener_restore_cost[2];
  int sgrproj_restore_cost[2];

  int tmp_pred_cost[SEG_TEMPORAL_PRED_CTXS][2];
  int spatial_pred_cost[SPATIAL_PREDICTION_PROBS][MAX_SEGMENTS];
} ModeCosts;

/*! \brief Holds mv costs for encoding and motion search.
 */
typedef struct {
  int nmv_joint_cost[MV_JOINTS];

  int nmv_cost_alloc[2][MV_VALS];
  int nmv_cost_hp_alloc[2][MV_VALS];
  int *nmv_cost[2];
  int *nmv_cost_hp[2];
  int **mv_cost_stack;
} MvCosts;

/*! \brief Holds mv costs for intrabc.
 */
typedef struct {
  /*! Costs for coding the joint mv. */
  int joint_mv[MV_JOINTS];

  /*! \brief Cost of transmitting the actual motion vector.
   *  dv_costs_alloc[0][i] is the cost of motion vector with horizontal
   * component (mv_row) equal to i - MV_MAX. dv_costs_alloc[1][i] is the cost of
   * motion vector with vertical component (mv_col) equal to i - MV_MAX.
   */
  int dv_costs_alloc[2][MV_VALS];

  /*! Points to the middle of \ref dv_costs_alloc. */
  int *dv_costs[2];
} IntraBCMVCosts;

/*! \brief Holds the costs needed to encode the coefficients
 */
typedef struct {
  LV_MAP_COEFF_COST coeff_costs[TX_SIZES][PLANE_TYPES];
  LV_MAP_EOB_COST eob_costs[7][2];
} CoeffCosts;

/*!\cond */
#define SINGLE_REF_MODES ((REF_FRAMES - 1) * 4)
/*!\endcond */
struct inter_modes_info;

/*! \brief Holds the motion samples for warp motion model estimation
 */
typedef struct {
  int num;
  int pts[16];
  int pts_inref[16];
} WARP_SAMPLE_INFO;

/*!\cond */
typedef enum {
  kZeroSad = 0,
  kVeryLowSad = 1,
  kLowSad = 2,
  kMedSad = 3,
  kHighSad = 4
} SOURCE_SAD;

typedef struct {
  SOURCE_SAD source_sad_nonrd;
  SOURCE_SAD source_sad_rd;
  int lighting_change;
  int low_sumdiff;
} CONTENT_STATE_SB;

typedef struct {
  uint16_t abs_dx_abs_dy_sum;
  int8_t hist_bin_idx;
  bool is_dx_zero;
} PixelLevelGradientInfo;

typedef struct {
  double log_var;
  int var;
} Block4x4VarInfo;

#if !defined(NDEBUG)
typedef struct SetOffsetsLoc {
  int mi_row;
  int mi_col;
  BLOCK_SIZE bsize;
} SetOffsetsLoc;
#endif

/*!\endcond */

#define TOP_COMP_AVG_EST_RD_COUNT 5

/*! \brief Encoder's parameters related to the current coding block.
 *
 * This struct contains most of the information the encoder needs to encode the
 * current coding block. This includes the src and pred buffer, a copy of the
 * decoder's view of the current block, the txfm coefficients. This struct also
 * contains various buffers and data used to speed up the encoding process.
 */
typedef struct macroblock {
  /*! \brief Each of the encoding plane.
   *
   * An array holding the src buffer for each of plane of the current block. It
   * also contains the txfm and quantized txfm coefficients.
   */
  struct macroblock_plane plane[MAX_MB_PLANE];

  /*! \brief Decoder's view of current coding block.
   *
   * Contains the encoder's copy of what the decoder sees in the current block.
   * Most importantly, this struct contains pointers to mbmi that is used in
   * final bitstream packing.
   */
  MACROBLOCKD e_mbd;

  /*! \brief Derived coding information.
   *
   * Contains extra information not transmitted in the bitstream but are
   * derived. For example, this contains the stack of ref_mvs.
   */
  MB_MODE_INFO_EXT mbmi_ext;

  /*! \brief Finalized mbmi_ext for the whole frame.
   *
   * Contains the finalized info in mbmi_ext that gets used at the frame level
   * for bitstream packing.
   */
  MB_MODE_INFO_EXT_FRAME *mbmi_ext_frame;

  FRAME_CONTEXT *row_ctx;
  /*! \brief Entropy context for the current tile.
   *
   * This context will be used to update color_map_cdf pointer which would be
   * used during pack bitstream. For single thread and tile-multithreading case
   * this pointer will be same as xd->tile_ctx, but for the case of row-mt:
   * xd->tile_ctx will point to a temporary context while tile_pb_ctx will point
   * to the accurate tile context.
   */
  FRAME_CONTEXT *tile_pb_ctx;

  /*! \brief Buffer of transformed coefficients
   *
   * Points to cb_coef_buff in the AV1_COMP struct, which contains the finalized
   * coefficients. This is here to conveniently copy the best coefficients to
   * frame level for bitstream packing. Since CB_COEFF_BUFFER is allocated on a
   * superblock level, we need to combine it with cb_offset to get the proper
   * position for the current coding block.
   */
  CB_COEFF_BUFFER *cb_coef_buff;
  uint16_t cb_offset[PLANE_TYPES];

  OBMCBuffer obmc_buffer;
  PALETTE_BUFFER *palette_buffer;
  CompoundTypeRdBuffers comp_rd_buffer;
  CONV_BUF_TYPE *tmp_conv_dst;

  /*! \brief Temporary buffer to hold prediction.
   *
   * Points to a buffer that is used to hold temporary prediction results. This
   * is used in two ways:
   * - This is a temporary buffer used to ping-pong the prediction in
   *   handle_inter_mode.
   * - xd->tmp_obmc_bufs also points to this buffer, and is used in ombc
   *   prediction.
   */
  uint8_t *tmp_pred_bufs[2];

  /*!
   *  Buffer used for upsampled prediction.
   */
  uint8_t *upsample_pred;

  /*! \brief Quantization index for the current partition block.
   *
   * This is used to as the index to find quantization parameter for luma and
   * chroma transformed coefficients.
   */
  int qindex;

  /*! \brief Difference between frame-level qindex and current qindex.
   *
   *  This is used to track whether a non-zero delta for qindex is used at least
   *  once in the current frame.
   */
  int delta_qindex;

  /*! \brief Difference between frame-level qindex and qindex used to
   * compute rdmult (lambda).
   *
   * rdmult_delta_qindex is assigned the same as delta_qindex before qp sweep.
   * During qp sweep, delta_qindex is changed and used to calculate the actual
   * quant params, while rdmult_delta_qindex remains the same, and is used to
   * calculate the rdmult in "set_deltaq_rdmult".
   */
  int rdmult_delta_qindex;

  /*! \brief Current qindex (before being adjusted by delta_q_res) used to
   * derive rdmult_delta_qindex.
   */
  int rdmult_cur_qindex;

  /*! \brief Rate-distortion multiplier.
   *
   * The rd multiplier used to determine the rate-distortion trade-off. This is
   * roughly proportional to the inverse of q-index for a given frame, but this
   * can be manipulated for better rate-control. For example, in tune_ssim
   * mode, this is scaled by a factor related to the variance of the current
   * block.
   */
  int rdmult;

  int intra_sb_rdmult_modifier;

  double rb;

  int mb_energy;
  int sb_energy_level;

  ModeCosts mode_costs;

  MvCosts *mv_costs;

  /*! The rate needed to encode a new motion vector to the bitstream in intrabc
   *  mode.
   */
  IntraBCMVCosts *dv_costs;

  CoeffCosts coeff_costs;

  int errorperbit;
  int sadperbit;

  /*! \brief Skip mode for the segment
   *
   * A syntax element of the segmentation mode. In skip_block mode, all mvs are
   * set 0 and all txfms are skipped.
   */
  int seg_skip_block;

  /*! \brief Number of segment 1 blocks
   * Actual number of (4x4) blocks that were applied delta-q,
   * for segment 1.
   */
  int actual_num_seg1_blocks;

  /*!\brief Number of segment 2 blocks
   * Actual number of (4x4) blocks that were applied delta-q,
   * for segment 2.
   */
  int actual_num_seg2_blocks;

  /*!\brief Number of zero motion vectors
   */
  int cnt_zeromv;

  /*!\brief Flag to force zeromv-skip at superblock level, for nonrd path.
   *
   * 0/1 imply zeromv-skip is disabled/enabled. 2 implies that the blocks
   * in the superblock may be marked as zeromv-skip at block level.
   */
  int force_zeromv_skip_for_sb;

  /*!\brief Flag to force zeromv-skip at block level, for nonrd path.
   */
  int force_zeromv_skip_for_blk;

  /*! \brief Previous segment id for which qmatrices were updated.
   * This is used to bypass setting of qmatrices if no change in qindex.
   */
  int prev_segment_id;

  SuperBlockEnc sb_enc;

  /*! \brief Characteristics of the current superblock.
   *
   *  Characteristics like whether the block has high sad, low sad, etc. This is
   *  only used by av1 realtime mode.
   */
  CONTENT_STATE_SB content_state_sb;

  /*! \brief Sum absolute distortion of the predicted mv for each ref frame.
   *
   * This is used to measure how viable a reference frame is.
   */
  int pred_mv_sad[REF_FRAMES];
  /*! \brief The minimum of \ref pred_mv_sad.
   *
   * Index 0 stores the minimum \ref pred_mv_sad across past reference frames.
   * Index 1 stores the minimum \ref pred_mv_sad across future reference frames.
   */
  int best_pred_mv_sad[2];
  int pred_mv0_sad[REF_FRAMES];
  int pred_mv1_sad[REF_FRAMES];

  /*! \brief Disables certain ref frame pruning based on tpl.
   *
   * Determines whether a given ref frame is "good" based on data from the TPL
   * model. If so, this stops selective_ref frame from pruning the given ref
   * frame at block level.
   */
  uint8_t tpl_keep_ref_frame[REF_FRAMES];

  /*! \brief Warp motion samples buffer.
   *
   * Store the motion samples used for warp motion.
   */
  WARP_SAMPLE_INFO warp_sample_info[REF_FRAMES];

  /*! \brief Reference frames picked by the square subblocks in a superblock.
   *
   * Keeps track of ref frames that are selected by square partition blocks
   * within a superblock, in MI resolution. They can be used to prune ref frames
   * for rectangular blocks.
   */
  int picked_ref_frames_mask[MAX_MIB_SIZE * MAX_MIB_SIZE];

  /*! \brief Prune ref frames in real-time mode.
   *
   * Determines whether to prune reference frames in real-time mode. For the
   * most part, this is the same as nonrd_prune_ref_frame_search in
   * cpi->sf.rt_sf.nonrd_prune_ref_frame_search, but this can be selectively
   * turned off if the only frame available is GOLDEN_FRAME.
   */
  int nonrd_prune_ref_frame_search;

  PartitionSearchInfo part_search_info;

  /*! \brief Whether to disable some features to force a mode in current block.
   *
   * In some cases, our speed features can be overly aggressive and remove all
   * modes search in the superblock. When this happens, we set
   * must_find_valid_partition to 1 to reduce the number of speed features, and
   * recode the superblock again.
   */
  int must_find_valid_partition;

  /*! \brief Inter skip mode.
   *
   * Skip mode tries to use the closest forward and backward references for
   * inter prediction. Skip here means to skip transmitting the reference
   * frames, not to be confused with skip_txfm.
   */
  int skip_mode;

  /*! \brief Factors used for rd-thresholding.
   *
   * Determines a rd threshold to determine whether to continue searching the
   * current mode. If the current best rd is already <= threshold, then we skip
   * the current mode.
   */
  int thresh_freq_fact[BLOCK_SIZES_ALL][MAX_MODES];

  /*! \brief Tracks the winner modes in the current coding block.
   *
   * Winner mode is a two-pass strategy to find the best prediction mode. In the
   * first pass, we search the prediction modes with a limited set of txfm
   * options, and keep the top modes. These modes are called the winner modes.
   * In the second pass, we retry the winner modes with more thorough txfm
   * options.
   */
  WinnerModeStats *winner_mode_stats;
  int winner_mode_count;

  /*! \brief The model used for rd-estimation to avoid txfm
   *
   * These are for inter_mode_rd_model_estimation, which is another two pass
   * approach. In this speed feature, we collect data in the first couple frames
   * to build an rd model to estimate the rdcost of a prediction model based on
   * the residue error. Once enough data is collected, this speed feature uses
   * the estimated rdcost to find the most performant prediction mode. Then we
   * follow up with a second pass find the best transform for the mode.
   * Determines if one would go with reduced complexity transform block
   * search model to select prediction modes, or full complexity model
   * to select transform kernel.
   */
  TXFM_RD_MODEL rd_model;

  /*! \brief Stores the inter mode information needed to build an rd model.
   *
   * These are for inter_mode_rd_model_estimation, which is another two pass
   * approach. In this speed feature, we collect data in the first couple frames
   * to build an rd model to estimate the rdcost of a prediction model based on
   * the residue error. Once enough data is collected, this speed feature uses
   * the estimated rdcost to find the most performant prediction mode. Then we
   * follow up with a second pass find the best transform for the mode.
   */
  struct inter_modes_info *inter_modes_info;

  uint8_t compound_idx;

  COMP_RD_STATS comp_rd_stats[MAX_COMP_RD_STATS];
  int comp_rd_stats_idx;

  /*! \brief Whether to recompute the luma prediction.
   *
   * In interpolation search, we can usually skip recalculating the luma
   * prediction because it is already calculated by a previous predictor. This
   * flag signifies that some modes might have been skipped, so we need to
   * rebuild the prediction.
   */
  int recalc_luma_mc_data;

  /*! \brief Data structure to speed up intrabc search.
   *
   * Contains the hash table, hash function, and buffer used for intrabc.
   */
  IntraBCHashInfo intrabc_hash_info;

  /*! \brief Whether to reuse the mode stored in mb_mode_cache. */
  int use_mb_mode_cache;
  /*! \brief The mode to reuse during \ref av1_rd_pick_intra_mode_sb and
   *  \ref av1_rd_pick_inter_mode. */
  const MB_MODE_INFO *mb_mode_cache;
  /*! \brief Pointer to the buffer which caches gradient information.
   *
   * Pointer to the array of structures to store gradient information of each
   * pixel in a superblock. The buffer constitutes of MAX_SB_SQUARE pixel level
   * structures for each of the plane types (PLANE_TYPE_Y and PLANE_TYPE_UV).
   */
  PixelLevelGradientInfo *pixel_gradient_info;
  /*! \brief Flags indicating the availability of cached gradient info. */
  bool is_sb_gradient_cached[PLANE_TYPES];

  /*! \brief Flag to reuse predicted samples of inter block. */
  bool reuse_inter_pred;

  /*! \brief Context used to determine the initial step size in motion search.
   *
   * This context is defined as the \f$l_\inf\f$ norm of the best ref_mvs for
   * each frame.
   */
  unsigned int max_mv_context[REF_FRAMES];

  /*! \brief Limit for the range of motion vectors.
   *
   * These define limits to motion vector components to prevent them from
   * extending outside the UMV borders
   */
  FullMvLimits mv_limits;

  /*! \brief Buffer for storing the search site config.
   *
   * When resize mode or super resolution mode is on, the stride of the
   * reference frame does not always match what's specified in \ref
   * MotionVectorSearchParams::search_site_cfg. When his happens, we update the
   * search_sine_config buffer here and use it for motion search.
   */
  search_site_config search_site_cfg_buf[NUM_DISTINCT_SEARCH_METHODS];

  /*! \brief Parameters that control how motion search is done.
   *
   * Stores various txfm search related parameters such as txfm_type, txfm_size,
   * trellis eob search, etc.
   */
  TxfmSearchParams txfm_search_params;

  /*! \brief Results of the txfm searches that have been done.
   *
   * Caches old txfm search results and keeps the current txfm decisions to
   * facilitate rdopt.
   */
  TxfmSearchInfo txfm_search_info;

  /*! \brief Whether there is a strong color activity.
   *
   * Used in REALTIME coding mode to enhance the visual quality at the boundary
   * of moving color objects.
   */
  uint8_t color_sensitivity_sb[MAX_MB_PLANE - 1];
  uint8_t color_sensitivity_sb_g[MAX_MB_PLANE - 1];
  uint8_t color_sensitivity_sb_alt[MAX_MB_PLANE - 1];
  uint8_t color_sensitivity[MAX_MB_PLANE - 1];
  int64_t min_dist_inter_uv;

  int color_palette_thresh;

  int force_color_check_block_level;

  tran_low_t *dqcoeff_buf;

  unsigned int source_variance;
  int block_is_zero_sad;
  int sb_me_partition;
  int sb_me_block;
  int sb_col_scroll;
  int sb_row_scroll;
  int_mv sb_me_mv;
  int sb_force_fixed_part;
  unsigned int pred_sse[REF_FRAMES];
#if CONFIG_RT_ML_PARTITIONING
  DECLARE_ALIGNED(16, uint8_t, est_pred[128 * 128]);
#endif

  /*! \brief NONE partition evaluated for merge.
   *
   * In variance based partitioning scheme, NONE & SPLIT partitions are
   * evaluated to check the SPLIT can be merged as NONE. This flag signifies the
   * partition is evaluated in the scheme.
   */
  int try_merge_partition;

  /*! \brief Pointer to buffer which caches sub-block variances in a superblock.
   *
   *  Pointer to the array of structures to store source variance information of
   *  each 4x4 sub-block in a superblock. Block4x4VarInfo structure is used to
   *  store source variance and log of source variance of each 4x4 sub-block.
   */
  Block4x4VarInfo *src_var_info_of_4x4_sub_blocks;
#if !defined(NDEBUG)
  /*! \brief A hash to make sure av1_set_offsets is called */
  SetOffsetsLoc last_set_offsets_loc;
#endif

#if COLLECT_NONRD_PICK_MODE_STAT
  mode_search_stat_nonrd ms_stat_nonrd;
#endif

  /*!\brief Number of pixels in current thread that choose palette mode in the
   * fast encoding stage for screen content tool detemination.
   */
  int palette_pixels;

  /*! \brief Keep records of top no-split RD Costs of transform size search. */
  int64_t top_inter_tx_no_split_rd[MAX_TX_BLOCKS_IN_MAX_SB]
                                  [TOP_INTER_TX_NO_SPLIT_COUNT];

  /*!\brief Pointer to the structure which stores the statistics used by
   * sb-level multi-pass encoding.
   */
  struct SB_FIRST_PASS_STATS *sb_stats_cache;

  /*!\brief Pointer to the structure which stores the statistics used by
   * first-pass when superblock is searched twice consecutively.
   */
  struct SB_FIRST_PASS_STATS *sb_fp_stats;

  /*!\brief Array of best estimated RD Costs of compound average. */
  int64_t top_comp_avg_est_rd[TOP_COMP_AVG_EST_RD_COUNT];

#if CONFIG_PARTITION_SEARCH_ORDER
  /*!\brief Pointer to RD_STATS structure to be used in
   * av1_rd_partition_search().
   */
  RD_STATS *rdcost;
#endif

  /*! \brief Distance from bottom edge of the frame in pixels. */
  int pix_to_bottom_edge;

  /*! \brief Distance from right edge of the frame in pixels. */
  int pix_to_right_edge;
} MACROBLOCK;
#undef SINGLE_REF_MODES

/*!\cond */
static inline void zero_winner_mode_stats(BLOCK_SIZE bsize, int n_stats,
                                          WinnerModeStats *stats) {
  if (stats == NULL) return;

  const int block_height = block_size_high[bsize];
  const int block_width = block_size_wide[bsize];
  for (int i = 0; i < n_stats; ++i) {
    WinnerModeStats *const stat = &stats[i];
    memset(&stat->mbmi, 0, sizeof(stat->mbmi));
    memset(&stat->rd_cost, 0, sizeof(stat->rd_cost));
    memset(&stat->rd, 0, sizeof(stat->rd));
    memset(&stat->rate_y, 0, sizeof(stat->rate_y));
    memset(&stat->rate_uv, 0, sizeof(stat->rate_uv));
    memset(&stat->color_index_map, 0,
           block_width * block_height * sizeof(stat->color_index_map[0]));
    memset(&stat->mode_index, 0, sizeof(stat->mode_index));
  }
}

static inline int is_rect_tx_allowed_bsize(BLOCK_SIZE bsize) {
  static const char LUT[BLOCK_SIZES_ALL] = {
    0,  
    1,  
    1,  
    0,  
    1,  
    1,  
    0,  
    1,  
    1,  
    0,  
    1,  
    1,  
    0,  
    0,  
    0,  
    0,  
    1,  
    1,  
    1,  
    1,  
    1,  
    1,  
  };

  return LUT[bsize];
}

static inline int is_rect_tx_allowed(const MACROBLOCKD *xd,
                                     const MB_MODE_INFO *mbmi) {
  return is_rect_tx_allowed_bsize(mbmi->bsize) &&
         !xd->lossless[mbmi->segment_id];
}

static inline int tx_size_to_depth(TX_SIZE tx_size, BLOCK_SIZE bsize) {
  TX_SIZE ctx_size = max_txsize_rect_lookup[bsize];
  int depth = 0;
  while (tx_size != ctx_size) {
    depth++;
    ctx_size = sub_tx_size_map[ctx_size];
    assert(depth <= MAX_TX_DEPTH);
  }
  return depth;
}

static inline void set_blk_skip(uint8_t txb_skip[], int plane, int blk_idx,
                                int skip) {
  if (skip)
    txb_skip[blk_idx] |= 1UL << plane;
  else
    txb_skip[blk_idx] &= ~(1UL << plane);
#if !defined(NDEBUG)
  if (plane == 0) {
    txb_skip[blk_idx] |= 1UL << (1 + 4);
    txb_skip[blk_idx] |= 1UL << (2 + 4);
  }

  txb_skip[blk_idx] &= ~(1UL << (plane + 4));
#endif
}

static inline int is_blk_skip(uint8_t *txb_skip, int plane, int blk_idx) {
#if !defined(NDEBUG)
  assert(!(txb_skip[blk_idx] & (1UL << (plane + 4))));

  assert((txb_skip[blk_idx] & 0x88) == 0);
#endif
  return (txb_skip[blk_idx] >> plane) & 1;
}

/*!\endcond */

#if defined(__cplusplus)
}  
#endif

#endif
