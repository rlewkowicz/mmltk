/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VP8_COMMON_ENTROPYMV_H_)
#define VPX_VP8_COMMON_ENTROPYMV_H_

#include "treecoder.h"

#if defined(__cplusplus)
extern "C" {
#endif

enum {
  mv_max = 1023,             
  MVvals = (2 * mv_max) + 1, 
  mvfp_max = 255, 
  MVfpvals = (2 * mvfp_max) + 1, 

  mvlong_width = 10, 
  mvnum_short = 8,   


  mvpis_short = 0, 
  MVPsign,         
  MVPshort,        

  MVPbits = MVPshort + mvnum_short - 1, 
  MVPcount = MVPbits + mvlong_width     
};

typedef struct mv_context {
  vp8_prob prob[MVPcount]; 
} MV_CONTEXT;

extern const MV_CONTEXT vp8_mv_update_probs[2], vp8_default_mv_context[2];

#if defined(__cplusplus)
}  
#endif

#endif
