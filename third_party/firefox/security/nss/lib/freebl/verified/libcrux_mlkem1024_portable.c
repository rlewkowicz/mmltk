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

#include "libcrux_mlkem1024_portable.h"

#include "internal/libcrux_mlkem_portable.h"
#include "libcrux_core.h"

static void
decapsulate_e0(libcrux_ml_kem_types_MlKemPrivateKey_83 *private_key,
               libcrux_ml_kem_types_MlKemCiphertext_64 *ciphertext,
               uint8_t ret[32U])
{
    libcrux_ml_kem_ind_cca_decapsulate_620(private_key, ciphertext, ret);
}

void
libcrux_ml_kem_mlkem1024_portable_decapsulate(
    libcrux_ml_kem_types_MlKemPrivateKey_83 *private_key,
    libcrux_ml_kem_types_MlKemCiphertext_64 *ciphertext, uint8_t ret[32U])
{
    decapsulate_e0(private_key, ciphertext, ret);
}

static tuple_fa
encapsulate_8f(
    libcrux_ml_kem_types_MlKemPublicKey_64 *public_key, uint8_t *randomness)
{
    return libcrux_ml_kem_ind_cca_encapsulate_ca0(public_key, randomness);
}

tuple_fa
libcrux_ml_kem_mlkem1024_portable_encapsulate(
    libcrux_ml_kem_types_MlKemPublicKey_64 *public_key,
    uint8_t randomness[32U])
{
    return encapsulate_8f(public_key, randomness);
}

static libcrux_ml_kem_mlkem1024_MlKem1024KeyPair
generate_keypair_b4(
    uint8_t *randomness)
{
    return libcrux_ml_kem_ind_cca_generate_keypair_150(randomness);
}

libcrux_ml_kem_mlkem1024_MlKem1024KeyPair
libcrux_ml_kem_mlkem1024_portable_generate_key_pair(uint8_t randomness[64U])
{
    return generate_keypair_b4(randomness);
}

static KRML_MUSTINLINE bool
validate_private_key_6b(
    libcrux_ml_kem_types_MlKemPrivateKey_83 *private_key,
    libcrux_ml_kem_types_MlKemCiphertext_64 *ciphertext)
{
    return libcrux_ml_kem_ind_cca_validate_private_key_b5(private_key,
                                                          ciphertext);
}

bool
libcrux_ml_kem_mlkem1024_portable_validate_private_key(
    libcrux_ml_kem_types_MlKemPrivateKey_83 *private_key,
    libcrux_ml_kem_types_MlKemCiphertext_64 *ciphertext)
{
    return validate_private_key_6b(private_key, ciphertext);
}

static KRML_MUSTINLINE bool
validate_private_key_only_44(
    libcrux_ml_kem_types_MlKemPrivateKey_83 *private_key)
{
    return libcrux_ml_kem_ind_cca_validate_private_key_only_60(private_key);
}

bool
libcrux_ml_kem_mlkem1024_portable_validate_private_key_only(
    libcrux_ml_kem_types_MlKemPrivateKey_83 *private_key)
{
    return validate_private_key_only_44(private_key);
}

static KRML_MUSTINLINE bool
validate_public_key_44(uint8_t *public_key)
{
    return libcrux_ml_kem_ind_cca_validate_public_key_ff(public_key);
}

bool
libcrux_ml_kem_mlkem1024_portable_validate_public_key(
    libcrux_ml_kem_types_MlKemPublicKey_64 *public_key)
{
    return validate_public_key_44(public_key->value);
}
