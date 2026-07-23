// Copyright 2023 Google Inc.  All rights reserved.
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "google/protobuf/reflection_mode.h"

#include "google/protobuf/port_def.inc"

namespace google {
namespace protobuf {
namespace internal {

#if !defined(PROTOBUF_NO_THREADLOCAL)

PROTOBUF_CONSTINIT PROTOBUF_THREAD_LOCAL ReflectionMode
    ScopedReflectionMode::reflection_mode_ = ReflectionMode::kDefault;

#endif

}  
}  
}  

#include "google/protobuf/port_undef.inc"
