/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_UsingHint_h
#define vm_UsingHint_h

#include <stdint.h>  // uint8_t

namespace js {

enum class UsingHint : uint8_t {
  Sync,
  Async,
};

}  

#endif /* vm_UsingHint_h */
