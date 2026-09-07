/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VP9_ENCODER_VP9_BITSTREAM_H_)
#define VPX_VP9_ENCODER_VP9_BITSTREAM_H_

#if defined(__cplusplus)
extern "C" {
#endif

#include "vp9/encoder/vp9_encoder.h"

typedef struct VP9BitstreamWorkerData {
  uint8_t *dest;
  size_t dest_size;
  vpx_writer bit_writer;
  int tile_idx;
  unsigned int max_mv_magnitude;
  int interp_filter_selected[1][SWITCHABLE];
  DECLARE_ALIGNED(16, MACROBLOCKD, xd);
} VP9BitstreamWorkerData;

int vp9_get_refresh_mask(VP9_COMP *cpi);

void vp9_bitstream_encode_tiles_buffer_dealloc(VP9_COMP *const cpi);

void vp9_pack_bitstream(VP9_COMP *cpi, uint8_t *dest, size_t dest_size,
                        size_t *size);

static INLINE int vp9_preserve_existing_gf(VP9_COMP *cpi) {
  return cpi->refresh_golden_frame && cpi->rc.is_src_frame_alt_ref &&
         !cpi->use_svc;
}

#if defined(__cplusplus)
}  
#endif

#endif
