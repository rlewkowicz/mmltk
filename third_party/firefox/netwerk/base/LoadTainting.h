/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_LoadTainting_h
#define mozilla_LoadTainting_h

#include <stdint.h>

namespace mozilla {

enum class LoadTainting : uint8_t { Basic = 0, CORS = 1, Opaque = 2 };

}  

#endif  // mozilla_LoadTainting_h
