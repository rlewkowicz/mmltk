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


#ifndef AVUTIL_BUFFER_H
#define AVUTIL_BUFFER_H

#include <stddef.h>
#include <stdint.h>


typedef struct AVBuffer AVBuffer;

typedef struct AVBufferRef {
  AVBuffer* buffer;

  uint8_t* data;
  size_t size;
} AVBufferRef;

AVBufferRef* av_buffer_alloc(size_t size);

AVBufferRef* av_buffer_allocz(size_t size);

#define AV_BUFFER_FLAG_READONLY (1 << 0)

AVBufferRef* av_buffer_create(uint8_t* data, size_t size,
                              void (*free)(void* opaque, uint8_t* data),
                              void* opaque, int flags);

void av_buffer_default_free(void* opaque, uint8_t* data);

AVBufferRef* av_buffer_ref(const AVBufferRef* buf);

void av_buffer_unref(AVBufferRef** buf);

int av_buffer_is_writable(const AVBufferRef* buf);

void* av_buffer_get_opaque(const AVBufferRef* buf);

int av_buffer_get_ref_count(const AVBufferRef* buf);

int av_buffer_make_writable(AVBufferRef** buf);

int av_buffer_realloc(AVBufferRef** buf, size_t size);

int av_buffer_replace(AVBufferRef** dst, const AVBufferRef* src);



typedef struct AVBufferPool AVBufferPool;

AVBufferPool* av_buffer_pool_init(size_t size,
                                  AVBufferRef* (*alloc)(size_t size));

AVBufferPool* av_buffer_pool_init2(size_t size, void* opaque,
                                   AVBufferRef* (*alloc)(void* opaque,
                                                         size_t size),
                                   void (*pool_free)(void* opaque));

void av_buffer_pool_uninit(AVBufferPool** pool);

AVBufferRef* av_buffer_pool_get(AVBufferPool* pool);

void* av_buffer_pool_buffer_get_opaque(const AVBufferRef* ref);


#endif /* AVUTIL_BUFFER_H */
