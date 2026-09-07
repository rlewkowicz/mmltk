/*
 * Codec parameters public API
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

#ifndef AVCODEC_CODEC_PAR_H
#define AVCODEC_CODEC_PAR_H

#include <stdint.h>

#include "libavutil/avutil.h"
#include "libavutil/rational.h"
#include "libavutil/pixfmt.h"

#include "codec_id.h"


enum AVFieldOrder {
  AV_FIELD_UNKNOWN,
  AV_FIELD_PROGRESSIVE,
  AV_FIELD_TT,  
  AV_FIELD_BB,  
  AV_FIELD_TB,  
  AV_FIELD_BT,  
};

typedef struct AVCodecParameters {
  enum AVMediaType codec_type;
  enum AVCodecID codec_id;
  uint32_t codec_tag;

  uint8_t* extradata;
  int extradata_size;

  int format;

  int64_t bit_rate;

  int bits_per_coded_sample;

  int bits_per_raw_sample;

  int profile;
  int level;

  int width;
  int height;

  AVRational sample_aspect_ratio;

  enum AVFieldOrder field_order;

  enum AVColorRange color_range;
  enum AVColorPrimaries color_primaries;
  enum AVColorTransferCharacteristic color_trc;
  enum AVColorSpace color_space;
  enum AVChromaLocation chroma_location;

  int video_delay;

  uint64_t channel_layout;
  int channels;
  int sample_rate;
  int block_align;
  int frame_size;

  int initial_padding;
  int trailing_padding;
  int seek_preroll;
} AVCodecParameters;

AVCodecParameters* avcodec_parameters_alloc(void);

void avcodec_parameters_free(AVCodecParameters** par);

int avcodec_parameters_copy(AVCodecParameters* dst,
                            const AVCodecParameters* src);

int av_get_audio_frame_duration2(AVCodecParameters* par, int frame_bytes);


#endif  // AVCODEC_CODEC_PAR_H
