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

#include "av1/common/debugmodes.h"

#include <stdio.h>

#include "av1/common/av1_common_int.h"
#include "av1/common/blockd.h"
#include "av1/common/enums.h"


void av1_print_uncompressed_frame_header(const uint8_t *data, int size,
                                         const char *filename) {
  FILE *hdrFile = fopen(filename, "w");
  if (!hdrFile) return;
  fwrite(data, size, sizeof(uint8_t), hdrFile);

  uint8_t zero = 0;
  fseek(hdrFile, 1, SEEK_SET);
  fwrite(&zero, 1, sizeof(uint8_t), hdrFile);
  fclose(hdrFile);
}

void av1_print_frame_contexts(const FRAME_CONTEXT *fc, const char *filename) {
  FILE *fcFile = fopen(filename, "w");
  if (!fcFile) return;
  const uint16_t *fcp = (uint16_t *)fc;
  const unsigned int n_contexts = sizeof(FRAME_CONTEXT) / sizeof(uint16_t);
  unsigned int i;

  for (i = 0; i < n_contexts; ++i) fprintf(fcFile, "%d ", *fcp++);
  fclose(fcFile);
}
