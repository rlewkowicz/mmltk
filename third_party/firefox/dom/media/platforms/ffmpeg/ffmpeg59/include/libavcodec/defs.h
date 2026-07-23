/*
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

#ifndef AVCODEC_DEFS_H
#define AVCODEC_DEFS_H


#include <stdint.h>
#include <stdlib.h>

#define AV_INPUT_BUFFER_PADDING_SIZE 64

enum AVDiscard {
  AVDISCARD_NONE = -16,  
  AVDISCARD_DEFAULT =
      0,                 
  AVDISCARD_NONREF = 8,  
  AVDISCARD_BIDIR = 16,  
  AVDISCARD_NONINTRA = 24,  
  AVDISCARD_NONKEY = 32,    
  AVDISCARD_ALL = 48,       
};

enum AVAudioServiceType {
  AV_AUDIO_SERVICE_TYPE_MAIN = 0,
  AV_AUDIO_SERVICE_TYPE_EFFECTS = 1,
  AV_AUDIO_SERVICE_TYPE_VISUALLY_IMPAIRED = 2,
  AV_AUDIO_SERVICE_TYPE_HEARING_IMPAIRED = 3,
  AV_AUDIO_SERVICE_TYPE_DIALOGUE = 4,
  AV_AUDIO_SERVICE_TYPE_COMMENTARY = 5,
  AV_AUDIO_SERVICE_TYPE_EMERGENCY = 6,
  AV_AUDIO_SERVICE_TYPE_VOICE_OVER = 7,
  AV_AUDIO_SERVICE_TYPE_KARAOKE = 8,
  AV_AUDIO_SERVICE_TYPE_NB,  
};

typedef struct AVPanScan {
  /**
   * id
   * - encoding: Set by user.
   * - decoding: Set by libavcodec.
   */
  int id;

  /**
   * width and height in 1/16 pel
   * - encoding: Set by user.
   * - decoding: Set by libavcodec.
   */
  int width;
  int height;

  /**
   * position of the top left corner in 1/16 pel for up to 3 fields/frames
   * - encoding: Set by user.
   * - decoding: Set by libavcodec.
   */
  int16_t position[3][2];
} AVPanScan;

typedef struct AVCPBProperties {
  int64_t max_bitrate;
  int64_t min_bitrate;
  int64_t avg_bitrate;

  int64_t buffer_size;

  uint64_t vbv_delay;
} AVCPBProperties;

AVCPBProperties* av_cpb_properties_alloc(size_t* size);

typedef struct AVProducerReferenceTime {
  int64_t wallclock;
  int flags;
} AVProducerReferenceTime;

unsigned int av_xiphlacing(unsigned char* s, unsigned int v);

#endif  // AVCODEC_DEFS_H
