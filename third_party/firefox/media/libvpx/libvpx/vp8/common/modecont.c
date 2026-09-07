/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "entropy.h"

const int vp8_mode_contexts[6][4] = {
  { 
    7, 1, 1, 143 },
  { 
    14, 18, 14, 107 },
  { 
    135, 64, 57, 68 },
  { 
    60, 56, 128, 65 },
  { 
    159, 134, 128, 34 },
  { 
    234, 188, 128, 28 },
};
