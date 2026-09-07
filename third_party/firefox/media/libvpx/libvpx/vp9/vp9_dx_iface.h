/*
 *  Copyright (c) 2015 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VP9_VP9_DX_IFACE_H_)
#define VPX_VP9_VP9_DX_IFACE_H_

#include "vp9/decoder/vp9_decoder.h"

typedef vpx_codec_stream_info_t vp9_stream_info_t;

struct vpx_codec_alg_priv {
  vpx_codec_priv_t base;
  vpx_codec_dec_cfg_t cfg;
  vp9_stream_info_t si;
  VP9Decoder *pbi;
  void *user_priv;
  int postproc_cfg_set;
  vp8_postproc_cfg_t postproc_cfg;
  vpx_decrypt_cb decrypt_cb;
  void *decrypt_state;
  vpx_image_t img;
  int img_avail;
  int flushed;
  int invert_tile_order;
  int last_show_frame;  
  int byte_alignment;
  int skip_loop_filter;

  int need_resync;  
  BufferPool *buffer_pool;

  void *ext_priv;  
  vpx_get_frame_buffer_cb_fn_t get_ext_fb_cb;
  vpx_release_frame_buffer_cb_fn_t release_ext_fb_cb;

  int svc_decoding;
  int svc_spatial_layer;
  int row_mt;
  int lpf_opt;
};

#endif
