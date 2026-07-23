/*
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVUTIL_SAMPLEFMT_H
#define AVUTIL_SAMPLEFMT_H

#include "avutil.h"

enum AVSampleFormat {
    AV_SAMPLE_FMT_NONE = -1,
    AV_SAMPLE_FMT_U8,          
    AV_SAMPLE_FMT_S16,         
    AV_SAMPLE_FMT_S32,         
    AV_SAMPLE_FMT_FLT,         
    AV_SAMPLE_FMT_DBL,         

    AV_SAMPLE_FMT_U8P,         
    AV_SAMPLE_FMT_S16P,        
    AV_SAMPLE_FMT_S32P,        
    AV_SAMPLE_FMT_FLTP,        
    AV_SAMPLE_FMT_DBLP,        

    AV_SAMPLE_FMT_NB           
};

const char *av_get_sample_fmt_name(enum AVSampleFormat sample_fmt);

enum AVSampleFormat av_get_sample_fmt(const char *name);

char *av_get_sample_fmt_string(char *buf, int buf_size, enum AVSampleFormat sample_fmt);

#if FF_API_GET_BITS_PER_SAMPLE_FMT
attribute_deprecated
int av_get_bits_per_sample_fmt(enum AVSampleFormat sample_fmt);
#endif

int av_get_bytes_per_sample(enum AVSampleFormat sample_fmt);

int av_sample_fmt_is_planar(enum AVSampleFormat sample_fmt);

int av_samples_get_buffer_size(int *linesize, int nb_channels, int nb_samples,
                               enum AVSampleFormat sample_fmt, int align);

int av_samples_fill_arrays(uint8_t **audio_data, int *linesize, uint8_t *buf,
                           int nb_channels, int nb_samples,
                           enum AVSampleFormat sample_fmt, int align);

int av_samples_alloc(uint8_t **audio_data, int *linesize, int nb_channels,
                     int nb_samples, enum AVSampleFormat sample_fmt, int align);

#endif /* AVUTIL_SAMPLEFMT_H */
