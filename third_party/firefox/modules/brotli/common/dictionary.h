/* Copyright 2013 Google Inc. All Rights Reserved.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT
*/


#ifndef BROTLI_COMMON_DICTIONARY_H_
#define BROTLI_COMMON_DICTIONARY_H_

#include "platform.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

typedef struct BrotliDictionary {
  uint8_t size_bits_by_length[32];

  uint32_t offsets_by_length[32];

  size_t data_size;

  const uint8_t* data;
} BrotliDictionary;

BROTLI_COMMON_API const BrotliDictionary* BrotliGetDictionary(void);

BROTLI_COMMON_API void BrotliSetDictionaryData(const uint8_t* data);

#define BROTLI_MIN_DICTIONARY_WORD_LENGTH 4
#define BROTLI_MAX_DICTIONARY_WORD_LENGTH 24

#if defined(__cplusplus) || defined(c_plusplus)
}  
#endif

#endif  /* BROTLI_COMMON_DICTIONARY_H_ */
