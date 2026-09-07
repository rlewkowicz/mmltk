/*
 * SPDX-FileCopyrightText: 2025 Cryspen Sarl <info@cryspen.com>
 *
 * SPDX-License-Identifier: MIT or Apache-2.0
 *
 * This code was generated with the following revisions:
 * Charon: 667d2fc98984ff7f3df989c2367e6c1fa4a000e7
 * Eurydice: 2381cbc416ef2ad0b561c362c500bc84f36b6785
 * Karamel: 80f5435f2fc505973c469a4afcc8d875cddd0d8b
 * F*: 71d8221589d4d438af3706d89cb653cf53e18aab
 * Libcrux: 68dfed5a4a9e40277f62828471c029afed1ecdcc
 */

#ifndef libcrux_mlkem768_portable_H
#define libcrux_mlkem768_portable_H

#include "eurydice_glue.h"

#if defined(__cplusplus)
extern "C" {
#endif

#include "libcrux_core.h"

void libcrux_ml_kem_mlkem768_portable_decapsulate(
    libcrux_ml_kem_types_MlKemPrivateKey_d9 *private_key,
    libcrux_ml_kem_mlkem768_MlKem768Ciphertext *ciphertext, uint8_t ret[32U]);

tuple_c2 libcrux_ml_kem_mlkem768_portable_encapsulate(
    libcrux_ml_kem_types_MlKemPublicKey_30 *public_key,
    uint8_t randomness[32U]);

libcrux_ml_kem_mlkem768_MlKem768KeyPair
libcrux_ml_kem_mlkem768_portable_generate_key_pair(uint8_t randomness[64U]);

bool libcrux_ml_kem_mlkem768_portable_validate_private_key(
    libcrux_ml_kem_types_MlKemPrivateKey_d9 *private_key,
    libcrux_ml_kem_mlkem768_MlKem768Ciphertext *ciphertext);

bool libcrux_ml_kem_mlkem768_portable_validate_private_key_only(
    libcrux_ml_kem_types_MlKemPrivateKey_d9 *private_key);

bool libcrux_ml_kem_mlkem768_portable_validate_public_key(
    libcrux_ml_kem_types_MlKemPublicKey_30 *public_key);

#if defined(__cplusplus)
}
#endif

#define libcrux_mlkem768_portable_H_DEFINED
#endif /* libcrux_mlkem768_portable_H */
