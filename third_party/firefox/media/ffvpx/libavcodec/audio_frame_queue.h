/*
 * Audio Frame Queue
 * Copyright (c) 2012 Justin Ruggles
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

#ifndef AVCODEC_AUDIO_FRAME_QUEUE_H
#define AVCODEC_AUDIO_FRAME_QUEUE_H

#include "avcodec.h"

typedef struct AudioFrame {
    int64_t pts;
    int duration;
} AudioFrame;

typedef struct AudioFrameQueue {
    AVCodecContext *avctx;
    int remaining_delay;
    int remaining_samples;
    AudioFrame *frames;
    unsigned frame_count;
    unsigned frame_alloc;
} AudioFrameQueue;

void ff_af_queue_init(AVCodecContext *avctx, AudioFrameQueue *afq);

void ff_af_queue_close(AudioFrameQueue *afq);

int ff_af_queue_add(AudioFrameQueue *afq, const AVFrame *f);

void ff_af_queue_remove(AudioFrameQueue *afq, int nb_samples, int64_t *pts,
                        int64_t *duration);

#endif /* AVCODEC_AUDIO_FRAME_QUEUE_H */
