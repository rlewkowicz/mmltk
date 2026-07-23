/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_AVDCT_H
#define AVCODEC_AVDCT_H

#include "libavutil/opt.h"

typedef struct AVDCT {
    const AVClass *av_class;

    void (*idct)(int16_t *block );

    uint8_t idct_permutation[64];

    void (*fdct)(int16_t *block );


    int dct_algo;

    int idct_algo;

    void (*get_pixels)(int16_t *block ,
                       const uint8_t *pixels ,
                       ptrdiff_t line_size);

    int bits_per_sample;

    void (*get_pixels_unaligned)(int16_t *block ,
                       const uint8_t *pixels,
                       ptrdiff_t line_size);
} AVDCT;

AVDCT *avcodec_dct_alloc(void);
int avcodec_dct_init(AVDCT *);

const AVClass *avcodec_dct_get_class(void);

#endif /* AVCODEC_AVDCT_H */
