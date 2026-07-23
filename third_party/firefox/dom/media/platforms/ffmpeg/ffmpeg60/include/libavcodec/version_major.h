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

#ifndef AVCODEC_VERSION_MAJOR_H
#define AVCODEC_VERSION_MAJOR_H


#define LIBAVCODEC_VERSION_MAJOR 60


#define FF_API_INIT_PACKET (LIBAVCODEC_VERSION_MAJOR < 61)
#define FF_API_IDCT_NONE (LIBAVCODEC_VERSION_MAJOR < 61)
#define FF_API_SVTAV1_OPTS (LIBAVCODEC_VERSION_MAJOR < 61)
#define FF_API_AYUV_CODECID (LIBAVCODEC_VERSION_MAJOR < 61)
#define FF_API_VT_OUTPUT_CALLBACK (LIBAVCODEC_VERSION_MAJOR < 61)
#define FF_API_AVCODEC_CHROMA_POS (LIBAVCODEC_VERSION_MAJOR < 61)
#define FF_API_VT_HWACCEL_CONTEXT (LIBAVCODEC_VERSION_MAJOR < 61)
#define FF_API_AVCTX_FRAME_NUMBER (LIBAVCODEC_VERSION_MAJOR < 61)

#define FF_CODEC_CRYSTAL_HD (LIBAVCODEC_VERSION_MAJOR < 61)

#endif /* AVCODEC_VERSION_MAJOR_H */
