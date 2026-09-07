/* Copyright 2014 Google Inc. All Rights Reserved.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT
*/


#ifndef WOFF2_WOFF2_DEC_H_
#define WOFF2_WOFF2_DEC_H_

#include <stddef.h>
#include <inttypes.h>
#include <woff2/output.h>

namespace woff2 {

size_t ComputeWOFF2FinalSize(const uint8_t *data, size_t length);

bool ConvertWOFF2ToTTF(uint8_t *result, size_t result_length,
                       const uint8_t *data, size_t length);

bool ConvertWOFF2ToTTF(const uint8_t *data, size_t length,
                       WOFF2Out* out);

} 

#endif  // WOFF2_WOFF2_DEC_H_
