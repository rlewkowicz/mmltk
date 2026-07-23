// Copyright 2008 Google Inc.  All rights reserved.
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "google/protobuf/raw_ptr.h"

#include <algorithm>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"

#include "google/protobuf/port_def.inc"

namespace google {
namespace protobuf {
namespace internal {

ABSL_CONST_INIT PROTOBUF_EXPORT ABSL_CACHELINE_ALIGNED const char
    kZeroBuffer[std::max(ABSL_CACHELINE_SIZE, 64)] = {};

}  
}  
}  

#include "google/protobuf/port_undef.inc"
