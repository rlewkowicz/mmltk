/**********************************************************************
  Copyright(c) 2011-2023 Intel Corporation All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********************************************************************/

#ifndef RAPIDGZIP_ISAL_CRC64_H_
#define RAPIDGZIP_ISAL_CRC64_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// All CRC64 variants share one signature; declare them via a single generator.
#define CRC64_DECLARE(name) uint64_t name(uint64_t init_crc, const unsigned char* buf, uint64_t len);

CRC64_DECLARE(crc64_ecma_refl)
CRC64_DECLARE(crc64_ecma_norm)
CRC64_DECLARE(crc64_iso_refl)
CRC64_DECLARE(crc64_iso_norm)
CRC64_DECLARE(crc64_jones_refl)
CRC64_DECLARE(crc64_jones_norm)
CRC64_DECLARE(crc64_rocksoft_refl)
CRC64_DECLARE(crc64_rocksoft_norm)
CRC64_DECLARE(crc64_ecma_refl_by8)
CRC64_DECLARE(crc64_ecma_norm_by8)
CRC64_DECLARE(crc64_ecma_refl_base)
CRC64_DECLARE(crc64_ecma_norm_base)
CRC64_DECLARE(crc64_iso_refl_by8)
CRC64_DECLARE(crc64_iso_norm_by8)
CRC64_DECLARE(crc64_iso_refl_base)
CRC64_DECLARE(crc64_iso_norm_base)
CRC64_DECLARE(crc64_jones_refl_by8)
CRC64_DECLARE(crc64_jones_norm_by8)
CRC64_DECLARE(crc64_jones_refl_base)
CRC64_DECLARE(crc64_jones_norm_base)
CRC64_DECLARE(crc64_rocksoft_refl_by8)
CRC64_DECLARE(crc64_rocksoft_refl_base)
CRC64_DECLARE(crc64_rocksoft_norm_by8)
CRC64_DECLARE(crc64_rocksoft_norm_base)

#ifdef __cplusplus
}
#endif

#endif  // _CRC64_H_
