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


#define LIBAVCODEC_VERSION_MAJOR 61


#define FF_API_INIT_PACKET (LIBAVCODEC_VERSION_MAJOR < 62)
#define FF_API_SUBFRAMES (LIBAVCODEC_VERSION_MAJOR < 62)
#define FF_API_TICKS_PER_FRAME (LIBAVCODEC_VERSION_MAJOR < 62)
#define FF_API_DROPCHANGED (LIBAVCODEC_VERSION_MAJOR < 62)

#define FF_API_AVFFT (LIBAVCODEC_VERSION_MAJOR < 62)
#define FF_API_FF_PROFILE_LEVEL (LIBAVCODEC_VERSION_MAJOR < 62)
#define FF_API_AVCODEC_CLOSE (LIBAVCODEC_VERSION_MAJOR < 62)
#define FF_API_BUFFER_MIN_SIZE (LIBAVCODEC_VERSION_MAJOR < 62)
#define FF_API_VDPAU_ALLOC_GET_SET (LIBAVCODEC_VERSION_MAJOR < 62)
#define FF_API_QUALITY_FACTOR (LIBAVCODEC_VERSION_MAJOR < 62)

#endif /* AVCODEC_VERSION_MAJOR_H */
