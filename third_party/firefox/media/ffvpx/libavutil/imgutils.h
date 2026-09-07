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

#ifndef AVUTIL_IMGUTILS_H
#define AVUTIL_IMGUTILS_H


#include <stddef.h>
#include <stdint.h>
#include "pixdesc.h"
#include "pixfmt.h"
#include "rational.h"

void av_image_fill_max_pixsteps(int max_pixsteps[4], int max_pixstep_comps[4],
                                const AVPixFmtDescriptor *pixdesc);

int av_image_get_linesize(enum AVPixelFormat pix_fmt, int width, int plane);

int av_image_fill_linesizes(int linesizes[4], enum AVPixelFormat pix_fmt, int width);

int av_image_fill_plane_sizes(size_t size[4], enum AVPixelFormat pix_fmt,
                              int height, const ptrdiff_t linesizes[4]);

int av_image_fill_pointers(uint8_t *data[4], enum AVPixelFormat pix_fmt, int height,
                           uint8_t *ptr, const int linesizes[4]);

int av_image_alloc(uint8_t *pointers[4], int linesizes[4],
                   int w, int h, enum AVPixelFormat pix_fmt, int align);

void av_image_copy_plane(uint8_t       *dst, int dst_linesize,
                         const uint8_t *src, int src_linesize,
                         int bytewidth, int height);

void av_image_copy_plane_uc_from(uint8_t       *dst, ptrdiff_t dst_linesize,
                                 const uint8_t *src, ptrdiff_t src_linesize,
                                 ptrdiff_t bytewidth, int height);

void av_image_copy(uint8_t * const dst_data[4], const int dst_linesizes[4],
                   const uint8_t * const src_data[4], const int src_linesizes[4],
                   enum AVPixelFormat pix_fmt, int width, int height);

static inline
void av_image_copy2(uint8_t * const dst_data[4], const int dst_linesizes[4],
                    uint8_t * const src_data[4], const int src_linesizes[4],
                    enum AVPixelFormat pix_fmt, int width, int height)
{
    av_image_copy(dst_data, dst_linesizes,
                  (const uint8_t * const *)src_data, src_linesizes,
                  pix_fmt, width, height);
}

void av_image_copy_uc_from(uint8_t * const dst_data[4],       const ptrdiff_t dst_linesizes[4],
                           const uint8_t * const src_data[4], const ptrdiff_t src_linesizes[4],
                           enum AVPixelFormat pix_fmt, int width, int height);

int av_image_fill_arrays(uint8_t *dst_data[4], int dst_linesize[4],
                         const uint8_t *src,
                         enum AVPixelFormat pix_fmt, int width, int height, int align);

int av_image_get_buffer_size(enum AVPixelFormat pix_fmt, int width, int height, int align);

int av_image_copy_to_buffer(uint8_t *dst, int dst_size,
                            const uint8_t * const src_data[4], const int src_linesize[4],
                            enum AVPixelFormat pix_fmt, int width, int height, int align);

int av_image_check_size(unsigned int w, unsigned int h, int log_offset, void *log_ctx);

int av_image_check_size2(unsigned int w, unsigned int h, int64_t max_pixels, enum AVPixelFormat pix_fmt, int log_offset, void *log_ctx);

int av_image_check_sar(unsigned int w, unsigned int h, AVRational sar);

int av_image_fill_black(uint8_t * const dst_data[4], const ptrdiff_t dst_linesize[4],
                        enum AVPixelFormat pix_fmt, enum AVColorRange range,
                        int width, int height);

int av_image_fill_color(uint8_t * const dst_data[4], const ptrdiff_t dst_linesize[4],
                        enum AVPixelFormat pix_fmt, const uint32_t color[4],
                        int width, int height, int flags);



#endif /* AVUTIL_IMGUTILS_H */
