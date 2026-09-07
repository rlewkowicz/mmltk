/*
 *  Copyright 2011 The LibYuv Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS. All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "libyuv/video_common.h"

#if defined(__cplusplus)
namespace libyuv {
extern "C" {
#endif

struct FourCCAliasEntry {
  uint32_t alias;
  uint32_t canonical;
};

#define NUM_ALIASES 18
static const struct FourCCAliasEntry kFourCCAliases[NUM_ALIASES] = {
    {FOURCC_IYUV, FOURCC_I420},
    {FOURCC_YU12, FOURCC_I420},
    {FOURCC_YU16, FOURCC_I422},
    {FOURCC_YU24, FOURCC_I444},
    {FOURCC_YUYV, FOURCC_YUY2},
    {FOURCC_YUVS, FOURCC_YUY2},  
    {FOURCC_HDYC, FOURCC_UYVY},
    {FOURCC_2VUY, FOURCC_UYVY},  
    {FOURCC_JPEG, FOURCC_MJPG},  
    {FOURCC_DMB1, FOURCC_MJPG},
    {FOURCC_BA81, FOURCC_BGGR},  
    {FOURCC_RGB3, FOURCC_RAW},
    {FOURCC_BGR3, FOURCC_24BG},
    {FOURCC_CM32, FOURCC_BGRA},  
    {FOURCC_CM24, FOURCC_RAW},   
    {FOURCC_L555, FOURCC_RGBO},  
    {FOURCC_L565, FOURCC_RGBP},  
    {FOURCC_5551, FOURCC_RGBO},  
};

LIBYUV_API
uint32_t CanonicalFourCC(uint32_t fourcc) {
  int i;
  for (i = 0; i < NUM_ALIASES; ++i) {
    if (kFourCCAliases[i].alias == fourcc) {
      return kFourCCAliases[i].canonical;
    }
  }
  return fourcc;
}

#if defined(__cplusplus)
}  
}  
#endif
