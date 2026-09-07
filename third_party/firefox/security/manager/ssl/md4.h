/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef md4_h_
#define md4_h_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void md4sum(const uint8_t* input, uint32_t inputLen, uint8_t* result);

#ifdef __cplusplus
}
#endif

#endif /* md4_h_ */
