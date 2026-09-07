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

#ifndef AVCODEC_PTHREAD_INTERNAL_H
#define AVCODEC_PTHREAD_INTERNAL_H

#include "avcodec.h"

#define MAX_AUTO_THREADS 16

int ff_slice_thread_init(AVCodecContext *avctx);
void ff_slice_thread_free(AVCodecContext *avctx);

int ff_frame_thread_init(AVCodecContext *avctx);
void ff_frame_thread_free(AVCodecContext *avctx, int thread_count);

#define THREAD_SENTINEL 0 // This forbids putting a mutex/condition variable at the front.
int  ff_pthread_init(void *obj, const unsigned offsets[]);
void ff_pthread_free(void *obj, const unsigned offsets[]);

#define OFFSET_ARRAY(...) __VA_ARGS__, THREAD_SENTINEL
#define DEFINE_OFFSET_ARRAY(type, name, cnt_variable, mutexes, conds)         \
static const unsigned name ## _offsets[] = { offsetof(type, cnt_variable),    \
                                             OFFSET_ARRAY mutexes,            \
                                             OFFSET_ARRAY conds }

#endif // AVCODEC_PTHREAD_INTERNAL_H
