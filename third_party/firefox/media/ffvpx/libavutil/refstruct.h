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

#ifndef AVUTIL_REFSTRUCT_H
#define AVUTIL_REFSTRUCT_H

#include <stddef.h>


typedef union {
    void *nc;
    const void *c;
} AVRefStructOpaque;

#define AV_REFSTRUCT_FLAG_NO_ZEROING (1 << 0)

void *av_refstruct_alloc_ext_c(size_t size, unsigned flags, AVRefStructOpaque opaque,
                               void (*free_cb)(AVRefStructOpaque opaque, void *obj));

static inline
void *av_refstruct_alloc_ext(size_t size, unsigned flags, void *opaque,
                             void (*free_cb)(AVRefStructOpaque opaque, void *obj))
{
    return av_refstruct_alloc_ext_c(size, flags, (AVRefStructOpaque){.nc = opaque},
                                    free_cb);
}

static inline
void *av_refstruct_allocz(size_t size)
{
    return av_refstruct_alloc_ext(size, 0, NULL, NULL);
}

void av_refstruct_unref(void *objp);

void *av_refstruct_ref(void *obj);

const void *av_refstruct_ref_c(const void *obj);

void av_refstruct_replace(void *dstp, const void *src);

int av_refstruct_exclusive(const void *obj);


typedef struct AVRefStructPool AVRefStructPool;

#define AV_REFSTRUCT_POOL_FLAG_NO_ZEROING         AV_REFSTRUCT_FLAG_NO_ZEROING
#define AV_REFSTRUCT_POOL_FLAG_RESET_ON_INIT_ERROR                   (1 << 16)
#define AV_REFSTRUCT_POOL_FLAG_FREE_ON_INIT_ERROR                    (1 << 17)
#define AV_REFSTRUCT_POOL_FLAG_ZERO_EVERY_TIME                       (1 << 18)

AVRefStructPool *av_refstruct_pool_alloc(size_t size, unsigned flags);

AVRefStructPool *av_refstruct_pool_alloc_ext_c(size_t size, unsigned flags,
                                               AVRefStructOpaque opaque,
                                               int  (*init_cb)(AVRefStructOpaque opaque, void *obj),
                                               void (*reset_cb)(AVRefStructOpaque opaque, void *obj),
                                               void (*free_entry_cb)(AVRefStructOpaque opaque, void *obj),
                                               void (*free_cb)(AVRefStructOpaque opaque));

static inline
AVRefStructPool *av_refstruct_pool_alloc_ext(size_t size, unsigned flags,
                                             void *opaque,
                                             int  (*init_cb)(AVRefStructOpaque opaque, void *obj),
                                             void (*reset_cb)(AVRefStructOpaque opaque, void *obj),
                                             void (*free_entry_cb)(AVRefStructOpaque opaque, void *obj),
                                             void (*free_cb)(AVRefStructOpaque opaque))
{
    return av_refstruct_pool_alloc_ext_c(size, flags, (AVRefStructOpaque){.nc = opaque},
                                         init_cb, reset_cb, free_entry_cb, free_cb);
}

void *av_refstruct_pool_get(AVRefStructPool *pool);

static inline void av_refstruct_pool_uninit(AVRefStructPool **poolp)
{
    av_refstruct_unref(poolp);
}

#endif /* AVUTIL_REFSTRUCT_H */
