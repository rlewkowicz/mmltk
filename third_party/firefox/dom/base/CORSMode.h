/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CORSMode_h_
#define CORSMode_h_

#include <stdint.h>

namespace mozilla {

enum CORSMode : uint8_t {
  CORS_NONE,

  CORS_ANONYMOUS,

  CORS_USE_CREDENTIALS
};

constexpr auto kFirstCORSMode = CORS_NONE;
constexpr auto kLastCORSMode = CORS_USE_CREDENTIALS;

}  

#endif /* CORSMode_h_ */
