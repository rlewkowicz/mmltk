/*
 *  Copyright (c) 2011 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#if !defined(VPX_VP8_ENCODER_LOOKAHEAD_H_)
#define VPX_VP8_ENCODER_LOOKAHEAD_H_
#include "vpx_scale/yv12config.h"
#include "vpx/vpx_integer.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct lookahead_entry {
  YV12_BUFFER_CONFIG img;
  int64_t ts_start;
  int64_t ts_end;
  unsigned int flags;
};

struct lookahead_ctx;

struct lookahead_ctx *vp8_lookahead_init(unsigned int width,
                                         unsigned int height,
                                         unsigned int depth);

void vp8_lookahead_destroy(struct lookahead_ctx *ctx);

int vp8_lookahead_push(struct lookahead_ctx *ctx, YV12_BUFFER_CONFIG *src,
                       int64_t ts_start, int64_t ts_end, unsigned int flags,
                       unsigned char *active_map);

struct lookahead_entry *vp8_lookahead_pop(struct lookahead_ctx *ctx, int drain);

#define PEEK_FORWARD 1
#define PEEK_BACKWARD (-1)
struct lookahead_entry *vp8_lookahead_peek(struct lookahead_ctx *ctx,
                                           unsigned int index, int direction);

unsigned int vp8_lookahead_depth(struct lookahead_ctx *ctx);

#if defined(__cplusplus)
}  
#endif

#endif
