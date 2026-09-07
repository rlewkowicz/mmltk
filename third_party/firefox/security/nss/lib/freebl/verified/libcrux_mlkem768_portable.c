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

#include "libcrux_mlkem768_portable.h"

#include "internal/libcrux_mlkem_portable.h"
#include "libcrux_core.h"

static void
decapsulate_35(
    libcrux_ml_kem_types_MlKemPrivateKey_d9 *private_key,
    libcrux_ml_kem_mlkem768_MlKem768Ciphertext *ciphertext, uint8_t ret[32U])
{
    libcrux_ml_kem_ind_cca_decapsulate_62(private_key, ciphertext, ret);
}

void
libcrux_ml_kem_mlkem768_portable_decapsulate(
    libcrux_ml_kem_types_MlKemPrivateKey_d9 *private_key,
    libcrux_ml_kem_mlkem768_MlKem768Ciphertext *ciphertext, uint8_t ret[32U])
{
    decapsulate_35(private_key, ciphertext, ret);
}

static tuple_c2
encapsulate_cd(
    libcrux_ml_kem_types_MlKemPublicKey_30 *public_key, uint8_t *randomness)
{
    return libcrux_ml_kem_ind_cca_encapsulate_ca(public_key, randomness);
}

tuple_c2
libcrux_ml_kem_mlkem768_portable_encapsulate(
    libcrux_ml_kem_types_MlKemPublicKey_30 *public_key,
    uint8_t randomness[32U])
{
    return encapsulate_cd(public_key, randomness);
}

static libcrux_ml_kem_mlkem768_MlKem768KeyPair
generate_keypair_ce(
    uint8_t *randomness)
{
    return libcrux_ml_kem_ind_cca_generate_keypair_15(randomness);
}

libcrux_ml_kem_mlkem768_MlKem768KeyPair
libcrux_ml_kem_mlkem768_portable_generate_key_pair(uint8_t randomness[64U])
{
    return generate_keypair_ce(randomness);
}

static KRML_MUSTINLINE bool
validate_private_key_31(
    libcrux_ml_kem_types_MlKemPrivateKey_d9 *private_key,
    libcrux_ml_kem_mlkem768_MlKem768Ciphertext *ciphertext)
{
    return libcrux_ml_kem_ind_cca_validate_private_key_37(private_key,
                                                          ciphertext);
}

bool
libcrux_ml_kem_mlkem768_portable_validate_private_key(
    libcrux_ml_kem_types_MlKemPrivateKey_d9 *private_key,
    libcrux_ml_kem_mlkem768_MlKem768Ciphertext *ciphertext)
{
    return validate_private_key_31(private_key, ciphertext);
}

static KRML_MUSTINLINE bool
validate_private_key_only_41(
    libcrux_ml_kem_types_MlKemPrivateKey_d9 *private_key)
{
    return libcrux_ml_kem_ind_cca_validate_private_key_only_d6(private_key);
}

bool
libcrux_ml_kem_mlkem768_portable_validate_private_key_only(
    libcrux_ml_kem_types_MlKemPrivateKey_d9 *private_key)
{
    return validate_private_key_only_41(private_key);
}

static KRML_MUSTINLINE bool
validate_public_key_41(uint8_t *public_key)
{
    return libcrux_ml_kem_ind_cca_validate_public_key_89(public_key);
}

bool
libcrux_ml_kem_mlkem768_portable_validate_public_key(
    libcrux_ml_kem_types_MlKemPublicKey_30 *public_key)
{
    return validate_public_key_41(public_key->value);
}
