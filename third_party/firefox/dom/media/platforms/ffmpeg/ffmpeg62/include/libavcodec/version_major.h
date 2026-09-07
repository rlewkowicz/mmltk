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


#define LIBAVCODEC_VERSION_MAJOR  62


#define FF_API_INIT_PACKET         (LIBAVCODEC_VERSION_MAJOR < 63)

#define FF_API_V408_CODECID        (LIBAVCODEC_VERSION_MAJOR < 63)
#define FF_API_CODEC_PROPS         (LIBAVCODEC_VERSION_MAJOR < 63)
#define FF_API_EXR_GAMMA           (LIBAVCODEC_VERSION_MAJOR < 63)
#define FF_API_INTRA_DC_PRECISION  (LIBAVCODEC_VERSION_MAJOR < 63)

#define FF_API_NVDEC_OLD_PIX_FMTS  (LIBAVCODEC_VERSION_MAJOR < 63)

#define FF_API_PARSER_PRIVATE      (LIBAVCODEC_VERSION_MAJOR < 63)
#define FF_API_PARSER_CODECID      (LIBAVCODEC_VERSION_MAJOR < 63)

#define FF_API_MJPEG_EXTERN_HUFF   (LIBAVCODEC_VERSION_MAJOR < 63)

#define FF_CODEC_OMX               (LIBAVCODEC_VERSION_MAJOR < 63)
#define FF_CODEC_SONIC_ENC         (LIBAVCODEC_VERSION_MAJOR < 63)
#define FF_CODEC_SONIC_DEC         (LIBAVCODEC_VERSION_MAJOR < 63)

#define FF_API_NVENC_H264_MAIN     (LIBAVCODEC_VERSION_MAJOR < 63)

#endif /* AVCODEC_VERSION_MAJOR_H */
