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

#if !defined(AOM_AV1_COMMON_TILE_COMMON_H_)
#define AOM_AV1_COMMON_TILE_COMMON_H_

#include <stdbool.h>

#include "config/aom_config.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct AV1Common;
struct SequenceHeader;
struct CommonTileParams;

#define DEFAULT_MAX_NUM_TG 1

typedef struct TileInfo {
  int mi_row_start, mi_row_end;
  int mi_col_start, mi_col_end;
  int tile_row;
  int tile_col;
} TileInfo;

void av1_tile_init(TileInfo *tile, const struct AV1Common *cm, int row,
                   int col);

void av1_tile_set_row(TileInfo *tile, const struct AV1Common *cm, int row);
void av1_tile_set_col(TileInfo *tile, const struct AV1Common *cm, int col);

int av1_get_sb_rows_in_tile(const struct AV1Common *cm, const TileInfo *tile);
int av1_get_sb_cols_in_tile(const struct AV1Common *cm, const TileInfo *tile);

#define MAX_TILE_WIDTH (4096)        // Max Tile width in pixels
#define MAX_TILE_AREA (4096 * 2304)  // Maximum tile area in pixels

bool av1_get_uniform_tile_size(const struct AV1Common *cm, int *w, int *h);
void av1_get_tile_limits(struct AV1Common *const cm);
void av1_calculate_tile_cols(const struct SequenceHeader *const seq_params,
                             int cm_mi_rows, int cm_mi_cols,
                             struct CommonTileParams *const tiles);
void av1_calculate_tile_rows(const struct SequenceHeader *const seq_params,
                             int cm_mi_rows,
                             struct CommonTileParams *const tiles);

int av1_is_min_tile_width_satisfied(const struct AV1Common *cm);

#if defined(__cplusplus)
}  
#endif

#endif
