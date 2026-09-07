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

#ifndef AVUTIL_BUFFER_INTERNAL_H
#define AVUTIL_BUFFER_INTERNAL_H

#include <stdatomic.h>
#include <stdint.h>

#include "buffer.h"
#include "thread.h"

#define BUFFER_FLAG_REALLOCATABLE (1 << 0)
#define BUFFER_FLAG_NO_FREE       (1 << 1)

struct AVBuffer {
    uint8_t *data; 
    size_t size; 

    atomic_uint refcount;

    void (*free)(void *opaque, uint8_t *data);

    void *opaque;

    int flags;

    int flags_internal;
};

typedef struct BufferPoolEntry {
    uint8_t *data;

    void *opaque;
    void (*free)(void *opaque, uint8_t *data);

    AVBufferPool *pool;
    struct BufferPoolEntry *next;

    AVBuffer buffer;
} BufferPoolEntry;

struct AVBufferPool {
    AVMutex mutex;
    BufferPoolEntry *pool;

    atomic_uint refcount;

    size_t size;
    void *opaque;
    AVBufferRef* (*alloc)(size_t size);
    AVBufferRef* (*alloc2)(void *opaque, size_t size);
    void         (*pool_free)(void *opaque);
};

#endif /* AVUTIL_BUFFER_INTERNAL_H */
