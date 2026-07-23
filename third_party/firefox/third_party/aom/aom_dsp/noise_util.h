/*
 * Copyright (c) 2017, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#if !defined(AOM_AOM_DSP_NOISE_UTIL_H_)
#define AOM_AOM_DSP_NOISE_UTIL_H_

#if defined(__cplusplus)
extern "C" {
#endif

struct aom_noise_tx_t;

struct aom_noise_tx_t *aom_noise_tx_malloc(int block_size);
void aom_noise_tx_free(struct aom_noise_tx_t *aom_noise_tx);

void aom_noise_tx_forward(struct aom_noise_tx_t *aom_noise_tx,
                          const float *data);

void aom_noise_tx_filter(struct aom_noise_tx_t *aom_noise_tx, const float *psd);

void aom_noise_tx_inverse(struct aom_noise_tx_t *aom_noise_tx, float *data);

void aom_noise_tx_add_energy(const struct aom_noise_tx_t *aom_noise_tx,
                             float *psd);

float aom_noise_psd_get_default_value(int block_size, float factor);

double aom_normalized_cross_correlation(const double *a, const double *b,
                                        int n);

int aom_noise_data_validate(const double *data, int w, int h);

#if defined(__cplusplus)
}  
#endif

#endif
