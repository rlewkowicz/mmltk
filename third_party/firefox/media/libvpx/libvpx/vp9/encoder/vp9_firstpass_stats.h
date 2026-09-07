/*
 *  Copyright (c) 2023 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VP9_ENCODER_VP9_FIRSTPASS_STATS_H_)
#define VPX_VP9_ENCODER_VP9_FIRSTPASS_STATS_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct {
  double frame;
  double weight;
  double intra_error;
  double coded_error;
  double sr_coded_error;
  double frame_noise_energy;
  double pcnt_inter;
  double pcnt_motion;
  double pcnt_second_ref;
  double pcnt_neutral;
  double pcnt_intra_low;   
  double pcnt_intra_high;  
  double intra_skip_pct;
  double intra_smooth_pct;    
  double inactive_zone_rows;  
  double inactive_zone_cols;  
  double MVr;
  double mvr_abs;
  double MVc;
  double mvc_abs;
  double MVrv;
  double MVcv;
  double mv_in_out_count;
  double duration;
  double count;
  double new_mv_count;
  int64_t spatial_layer_id;
} FIRSTPASS_STATS;

#if defined(__cplusplus)
}  
#endif

#endif
