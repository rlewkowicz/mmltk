/*
 * Codec descriptors public API
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

#ifndef AVCODEC_CODEC_DESC_H
#define AVCODEC_CODEC_DESC_H

#include "libavutil/avutil.h"

#include "codec_id.h"


typedef struct AVCodecDescriptor {
  enum AVCodecID id;
  enum AVMediaType type;
  const char* name;
  const char* long_name;
  int props;
  const char* const* mime_types;
  const struct AVProfile* profiles;
} AVCodecDescriptor;

#define AV_CODEC_PROP_INTRA_ONLY (1 << 0)
#define AV_CODEC_PROP_LOSSY (1 << 1)
#define AV_CODEC_PROP_LOSSLESS (1 << 2)
#define AV_CODEC_PROP_REORDER (1 << 3)
#define AV_CODEC_PROP_BITMAP_SUB (1 << 16)
#define AV_CODEC_PROP_TEXT_SUB (1 << 17)

const AVCodecDescriptor* avcodec_descriptor_get(enum AVCodecID id);

const AVCodecDescriptor* avcodec_descriptor_next(const AVCodecDescriptor* prev);

const AVCodecDescriptor* avcodec_descriptor_get_by_name(const char* name);


#endif  // AVCODEC_CODEC_DESC_H
