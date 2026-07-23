/*
 *  Copyright (c) 2023 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

/*!\file
 * \brief Describes the TPL stats descriptor and associated operations
 *
 */
#if !defined(VPX_VPX_VPX_TPL_H_)
#define VPX_VPX_VPX_TPL_H_

#include "./vpx_integer.h"
#include "./vpx_codec.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*!\brief Current ABI version number
 *
 * \internal
 * If this file is altered in any way that changes the ABI, this value
 * must be bumped.  Examples include, but are not limited to, changing
 * types, removing or reassigning enums, adding/removing/rearranging
 * fields to structures
 */
#define VPX_TPL_ABI_VERSION 5 /**<\hideinitializer*/

/*!\brief Temporal dependency model stats for each block before propagation */
typedef struct VpxTplBlockStats {
  int16_t row;            
  int16_t col;            
  int64_t intra_cost;     
  int64_t inter_cost;     
  int16_t mv_r;           
  int16_t mv_c;           
  int64_t srcrf_rate;     
  int64_t srcrf_dist;     
  int64_t pred_error;     
  int64_t inter_pred_err; 
  int64_t intra_pred_err; 
  int ref_frame_index;    
} VpxTplBlockStats;

/*!\brief Temporal dependency model stats for each frame before propagation */
typedef struct VpxTplFrameStats {
  int frame_width;  
  int frame_height; 
  int num_blocks;   
  VpxTplBlockStats *block_stats_list; 
} VpxTplFrameStats;

/*!\brief Temporal dependency model stats for each GOP before propagation */
typedef struct VpxTplGopStats {
  int size; 
  VpxTplFrameStats *frame_stats_list; 
} VpxTplGopStats;

#if defined(__cplusplus)
}  
#endif

#endif
