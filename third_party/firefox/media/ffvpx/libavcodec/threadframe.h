/*
 * Copyright (c) 2022 Andreas Rheinhardt <andreas.rheinhardt@outlook.com>
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

#ifndef AVCODEC_THREADFRAME_H
#define AVCODEC_THREADFRAME_H

#include "libavutil/frame.h"
#include "avcodec.h"

typedef struct ThreadFrame {
    AVFrame *f;
    AVCodecContext *owner[2];
    struct ThreadFrameProgress *progress;
} ThreadFrame;

void ff_thread_report_progress(ThreadFrame *f, int progress, int field);

void ff_thread_await_progress(const ThreadFrame *f, int progress, int field);

int ff_thread_get_ext_buffer(AVCodecContext *avctx, ThreadFrame *f, int flags);

void ff_thread_release_ext_buffer(ThreadFrame *f);

int ff_thread_ref_frame(ThreadFrame *dst, const ThreadFrame *src);

int ff_thread_replace_frame(ThreadFrame *dst, const ThreadFrame *src);

#endif
