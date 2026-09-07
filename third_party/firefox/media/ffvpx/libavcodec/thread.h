/*
 * Copyright (c) 2008 Alexander Strange <astrange@ithinksw.com>
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


#ifndef AVCODEC_THREAD_H
#define AVCODEC_THREAD_H

#include "libavutil/buffer.h"

#include "avcodec.h"

int ff_thread_can_start_frame(AVCodecContext *avctx);

void ff_thread_finish_setup(AVCodecContext *avctx);

int ff_thread_get_buffer(AVCodecContext *avctx, AVFrame *f, int flags);

int ff_slice_thread_execute_with_mainfunc(AVCodecContext *avctx,
        int (*action_func2)(AVCodecContext *c, void *arg, int jobnr, int threadnr),
        int (*main_func)(AVCodecContext *c), void *arg, int *ret, int job_count);

enum ThreadingStatus {
    FF_THREAD_IS_COPY,
    FF_THREAD_IS_FIRST_THREAD,
    FF_THREAD_NO_FRAME_THREADING,
};

enum ThreadingStatus ff_thread_sync_ref(AVCodecContext *avctx, size_t offset);

#endif /* AVCODEC_THREAD_H */
