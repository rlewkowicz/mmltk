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


#ifndef AVUTIL_FIFO_H
#define AVUTIL_FIFO_H

#include <stddef.h>


typedef struct AVFifo AVFifo;

typedef int AVFifoCB(void *opaque, void *buf, size_t *nb_elems);

#define AV_FIFO_FLAG_AUTO_GROW      (1 << 0)

AVFifo *av_fifo_alloc2(size_t elems, size_t elem_size,
                       unsigned int flags);

size_t av_fifo_elem_size(const AVFifo *f);

void av_fifo_auto_grow_limit(AVFifo *f, size_t max_elems);

size_t av_fifo_can_read(const AVFifo *f);

size_t av_fifo_can_write(const AVFifo *f);

int av_fifo_grow2(AVFifo *f, size_t inc);

int av_fifo_write(AVFifo *f, const void *buf, size_t nb_elems);

int av_fifo_write_from_cb(AVFifo *f, AVFifoCB read_cb,
                          void *opaque, size_t *nb_elems);

int av_fifo_read(AVFifo *f, void *buf, size_t nb_elems);

int av_fifo_read_to_cb(AVFifo *f, AVFifoCB write_cb,
                       void *opaque, size_t *nb_elems);

int av_fifo_peek(const AVFifo *f, void *buf, size_t nb_elems, size_t offset);

int av_fifo_peek_to_cb(const AVFifo *f, AVFifoCB write_cb, void *opaque,
                       size_t *nb_elems, size_t offset);

void av_fifo_drain2(AVFifo *f, size_t size);

void av_fifo_reset2(AVFifo *f);

void av_fifo_freep2(AVFifo **f);


#endif /* AVUTIL_FIFO_H */
