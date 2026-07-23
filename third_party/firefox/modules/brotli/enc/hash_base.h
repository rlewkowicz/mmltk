/* Copyright 2025 Google Inc. All Rights Reserved.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT
*/


#ifndef THIRD_PARTY_BROTLI_ENC_HASH_BASE_H_
#define THIRD_PARTY_BROTLI_ENC_HASH_BASE_H_

#include "../common/platform.h"

static const uint32_t kHashMul32 = 0x1E35A7BD;
static const uint64_t kHashMul64 =
    BROTLI_MAKE_UINT64_T(0x1FE35A7Bu, 0xD3579BD3u);

static BROTLI_INLINE uint32_t Hash14(const uint8_t* data) {
  uint32_t h = BROTLI_UNALIGNED_LOAD32LE(data) * kHashMul32;
  return h >> (32 - 14);
}

static BROTLI_INLINE uint32_t Hash15(const uint8_t* data) {
  uint32_t h = BROTLI_UNALIGNED_LOAD32LE(data) * kHashMul32;
  return h >> (32 - 15);
}

#endif  // THIRD_PARTY_BROTLI_ENC_HASH_BASE_H_
