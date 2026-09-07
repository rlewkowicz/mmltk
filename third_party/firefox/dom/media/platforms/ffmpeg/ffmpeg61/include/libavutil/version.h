/*
 * copyright (c) 2003 Fabrice Bellard
 *
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


#ifndef AVUTIL_VERSION_H
#define AVUTIL_VERSION_H

#include "macros.h"


#define AV_VERSION_INT(a, b, c) ((a) << 16 | (b) << 8 | (c))
#define AV_VERSION_DOT(a, b, c) a##.##b##.##c
#define AV_VERSION(a, b, c) AV_VERSION_DOT(a, b, c)

#define AV_VERSION_MAJOR(a) ((a) >> 16)
#define AV_VERSION_MINOR(a) (((a) & 0x00FF00) >> 8)
#define AV_VERSION_MICRO(a) ((a) & 0xFF)



#define LIBAVUTIL_VERSION_MAJOR 59
#define LIBAVUTIL_VERSION_MINOR 13
#define LIBAVUTIL_VERSION_MICRO 100

#define LIBAVUTIL_VERSION_INT                                      \
  AV_VERSION_INT(LIBAVUTIL_VERSION_MAJOR, LIBAVUTIL_VERSION_MINOR, \
                 LIBAVUTIL_VERSION_MICRO)
#define LIBAVUTIL_VERSION                                      \
  AV_VERSION(LIBAVUTIL_VERSION_MAJOR, LIBAVUTIL_VERSION_MINOR, \
             LIBAVUTIL_VERSION_MICRO)
#define LIBAVUTIL_BUILD LIBAVUTIL_VERSION_INT

#define LIBAVUTIL_IDENT "Lavu" AV_STRINGIFY(LIBAVUTIL_VERSION)


#define FF_API_HDR_VIVID_THREE_SPLINE (LIBAVUTIL_VERSION_MAJOR < 60)
#define FF_API_FRAME_PKT (LIBAVUTIL_VERSION_MAJOR < 60)
#define FF_API_INTERLACED_FRAME (LIBAVUTIL_VERSION_MAJOR < 60)
#define FF_API_FRAME_KEY (LIBAVUTIL_VERSION_MAJOR < 60)
#define FF_API_PALETTE_HAS_CHANGED (LIBAVUTIL_VERSION_MAJOR < 60)
#define FF_API_VULKAN_CONTIGUOUS_MEMORY (LIBAVUTIL_VERSION_MAJOR < 60)
#define FF_API_H274_FILM_GRAIN_VCS (LIBAVUTIL_VERSION_MAJOR < 60)


#endif /* AVUTIL_VERSION_H */
