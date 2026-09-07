/* MIT License
 *
 * Copyright (c) 2016-2022 INRIA, CMU and Microsoft Corporation
 * Copyright (c) 2022-2023 HACL* Contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __Hacl_P521_H
#define __Hacl_P521_H

#if defined(__cplusplus)
extern "C" {
#endif

#include <string.h>
#include "krml/internal/types.h"
#include "krml/lowstar_endianness.h"

#include "lib_intrinsics.h"



bool
Hacl_P521_ecdsa_sign_p521_without_hash(
    uint8_t *signature,
    uint32_t msg_len,
    uint8_t *msg,
    uint8_t *private_key,
    uint8_t *nonce);


bool
Hacl_P521_ecdsa_verif_without_hash(
    uint32_t msg_len,
    uint8_t *msg,
    uint8_t *public_key,
    uint8_t *signature_r,
    uint8_t *signature_s);


bool Hacl_P521_validate_public_key(uint8_t *public_key);

bool Hacl_P521_validate_private_key(uint8_t *private_key);


bool Hacl_P521_uncompressed_to_raw(uint8_t *pk, uint8_t *pk_raw);

bool Hacl_P521_compressed_to_raw(uint8_t *pk, uint8_t *pk_raw);

void Hacl_P521_raw_to_uncompressed(uint8_t *pk_raw, uint8_t *pk);

void Hacl_P521_raw_to_compressed(uint8_t *pk_raw, uint8_t *pk);


bool Hacl_P521_dh_initiator(uint8_t *public_key, uint8_t *private_key);

bool
Hacl_P521_dh_responder(uint8_t *shared_secret, uint8_t *their_pubkey, uint8_t *private_key);

#if defined(__cplusplus)
}
#endif

#define __Hacl_P521_H_DEFINED
#endif
