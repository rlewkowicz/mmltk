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

#ifndef AVCODEC_PROGRESSFRAME_H
#define AVCODEC_PROGRESSFRAME_H


struct AVCodecContext;

typedef struct ProgressFrame {
    struct AVFrame *f;
    struct ProgressInternal *progress;
} ProgressFrame;

void ff_progress_frame_report(ProgressFrame *f, int progress);

void ff_progress_frame_await(const ProgressFrame *f, int progress);

int ff_progress_frame_alloc(struct AVCodecContext *avctx, ProgressFrame *f);

int ff_progress_frame_get_buffer(struct AVCodecContext *avctx,
                                 ProgressFrame *f, int flags);

void ff_progress_frame_unref(ProgressFrame *f);

void ff_progress_frame_ref(ProgressFrame *dst, const ProgressFrame *src);

void ff_progress_frame_replace(ProgressFrame *dst, const ProgressFrame *src);

#endif /* AVCODEC_PROGRESSFRAME_H */
