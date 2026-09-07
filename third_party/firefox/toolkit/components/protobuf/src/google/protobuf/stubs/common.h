// Copyright 2008 Google Inc.  All rights reserved.
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd


#if !defined(GOOGLE_PROTOBUF_COMMON_H__)
#define GOOGLE_PROTOBUF_COMMON_H__

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "google/protobuf/stubs/platform_macros.h"
#include "google/protobuf/stubs/port.h"


#if 0 || 0 || \
    (0 && 0) ||             \
    defined(GOOGLE_PROTOBUF_OS_IPHONE)
#include <pthread.h>
#endif

#include "google/protobuf/port_def.inc"

namespace std {}

namespace google {
namespace protobuf {
namespace internal {


#define GOOGLE_PROTOBUF_VERSION 7035000

#define GOOGLE_PROTOBUF_VERSION_SUFFIX ""

void PROTOBUF_EXPORT VerifyVersion(int protobufVersionCompiledWith,
                                   const char* filename);

std::string PROTOBUF_EXPORT
VersionString(int version);  // NOLINT(runtime/string)

std::string PROTOBUF_EXPORT
ProtocVersionString(int version);  // NOLINT(runtime/string)

}  

#define GOOGLE_PROTOBUF_VERIFY_VERSION \
  ::google::protobuf::internal::VerifyVersion(GOOGLE_PROTOBUF_VERSION, __FILE__)

PROTOBUF_EXPORT void ShutdownProtobufLibrary();

namespace internal {

template <typename T>
void StrongReference(const T& var) {
  auto volatile unused = &var;
  (void)&unused;  
}

}  
}  
}  

#include "google/protobuf/port_undef.inc"

#endif
