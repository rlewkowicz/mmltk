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

#ifndef AVUTIL_CONTAINER_FIFO_H
#define AVUTIL_CONTAINER_FIFO_H

#include <stddef.h>

typedef struct AVContainerFifo AVContainerFifo;

enum AVContainerFifoFlags {
    AV_CONTAINER_FIFO_FLAG_REF  = (1 << 0),

    AV_CONTAINER_FIFO_FLAG_USER = (1 << 16),
};

AVContainerFifo*
av_container_fifo_alloc(void *opaque,
                        void* (*container_alloc)(void *opaque),
                        void  (*container_reset)(void *opaque, void *obj),
                        void  (*container_free) (void *opaque, void *obj),
                        int   (*fifo_transfer)  (void *opaque, void *dst, void *src, unsigned flags),
                        unsigned flags);

AVContainerFifo *av_container_fifo_alloc_avframe(unsigned flags);

void av_container_fifo_free(AVContainerFifo **cf);

int av_container_fifo_write(AVContainerFifo *cf, void *obj, unsigned flags);

int av_container_fifo_read(AVContainerFifo *cf, void *obj, unsigned flags);

int av_container_fifo_peek(AVContainerFifo *cf, void **pobj, size_t offset);

void av_container_fifo_drain(AVContainerFifo *cf, size_t nb_elems);

size_t av_container_fifo_can_read(const AVContainerFifo *cf);

#endif // AVCODEC_CONTAINER_FIFO_H
