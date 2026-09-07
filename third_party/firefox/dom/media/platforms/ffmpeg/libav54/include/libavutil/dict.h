/*
 *
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


#ifndef AVUTIL_DICT_H
#define AVUTIL_DICT_H


#define AV_DICT_MATCH_CASE      1
#define AV_DICT_IGNORE_SUFFIX   2
#define AV_DICT_DONT_STRDUP_KEY 4   /**< Take ownership of a key that's been
                                         allocated with av_malloc() and children. */
#define AV_DICT_DONT_STRDUP_VAL 8   /**< Take ownership of a value that's been
                                         allocated with av_malloc() and chilren. */
#define AV_DICT_DONT_OVERWRITE 16   ///< Don't overwrite existing entries.
#define AV_DICT_APPEND         32   /**< If the entry already exists, append to it.  Note that no
                                      delimiter is added, the strings are simply concatenated. */

typedef struct AVDictionaryEntry {
    char *key;
    char *value;
} AVDictionaryEntry;

typedef struct AVDictionary AVDictionary;

AVDictionaryEntry *
av_dict_get(AVDictionary *m, const char *key, const AVDictionaryEntry *prev, int flags);

int av_dict_count(const AVDictionary *m);

int av_dict_set(AVDictionary **pm, const char *key, const char *value, int flags);

void av_dict_copy(AVDictionary **dst, AVDictionary *src, int flags);

void av_dict_free(AVDictionary **m);


#endif /* AVUTIL_DICT_H */
