/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ScalarTypeUtils_h
#define jit_ScalarTypeUtils_h

#include "mozilla/CheckedInt.h"

#include <stdint.h>

#include "js/ScalarType.h"

namespace js {
namespace jit {

[[nodiscard]] inline bool ArrayOffsetFitsInInt32(int32_t index,
                                                 Scalar::Type type,
                                                 int32_t* offset) {
  mozilla::CheckedInt<int32_t> val = index;
  val *= Scalar::byteSize(type);
  if (!val.isValid() || val.value() < 0) {
    return false;
  }

  *offset = val.value();
  return true;
}

}  
}  

#endif /* jit_ScalarTypeUtils_h */
