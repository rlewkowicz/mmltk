// Copyright 2021 The Abseil Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_STRINGS_INTERNAL_CORD_REP_CRC_H_
#define ABSL_STRINGS_INTERNAL_CORD_REP_CRC_H_

#include <cassert>
#include <cstdint>

#include "absl/base/config.h"
#include "absl/base/optimization.h"
#include "absl/crc/internal/crc_cord_state.h"
#include "absl/strings/internal/cord_internal.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace cord_internal {

struct CordRepCrc : public CordRep {
  CordRep* child;
  absl::crc_internal::CrcCordState crc_cord_state;

  static CordRepCrc* New(CordRep* child, crc_internal::CrcCordState state);

  static void Destroy(CordRepCrc* node);
};

inline CordRep* RemoveCrcNode(CordRep* rep) {
  assert(rep != nullptr);
  if (ABSL_PREDICT_FALSE(rep->IsCrc())) {
    CordRep* child = rep->crc()->child;
    if (rep->refcount.IsOne()) {
      delete rep->crc();
    } else {
      CordRep::Ref(child);
      CordRep::Unref(rep);
    }
    return child;
  }
  return rep;
}

inline CordRep* SkipCrcNode(CordRep* rep) {
  assert(rep != nullptr);
  if (ABSL_PREDICT_FALSE(rep->IsCrc())) {
    return rep->crc()->child;
  } else {
    return rep;
  }
}

inline const CordRep* SkipCrcNode(const CordRep* rep) {
  assert(rep != nullptr);
  if (ABSL_PREDICT_FALSE(rep->IsCrc())) {
    return rep->crc()->child;
  } else {
    return rep;
  }
}

inline CordRepCrc* CordRep::crc() {
  assert(IsCrc());
  return static_cast<CordRepCrc*>(this);
}

inline const CordRepCrc* CordRep::crc() const {
  assert(IsCrc());
  return static_cast<const CordRepCrc*>(this);
}

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_INTERNAL_CORD_REP_CRC_H_
