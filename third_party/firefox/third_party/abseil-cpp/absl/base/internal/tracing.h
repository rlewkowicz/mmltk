// Copyright 2024 The Abseil Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_BASE_INTERNAL_TRACING_H_
#define ABSL_BASE_INTERNAL_TRACING_H_

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace base_internal {

enum class ObjectKind { kUnknown, kBlockingCounter, kNotification };

void TraceWait(const void* object, ObjectKind kind);
void TraceContinue(const void* object, ObjectKind kind);

void TraceSignal(const void* object, ObjectKind kind);

void TraceObserved(const void* object, ObjectKind kind);

extern "C" {

  void ABSL_INTERNAL_C_SYMBOL(AbslInternalTraceWait)(const void* object,
                                                     ObjectKind kind);
  void ABSL_INTERNAL_C_SYMBOL(AbslInternalTraceContinue)(const void* object,
                                                         ObjectKind kind);
  void ABSL_INTERNAL_C_SYMBOL(AbslInternalTraceSignal)(const void* object,
                                                       ObjectKind kind);
  void ABSL_INTERNAL_C_SYMBOL(AbslInternalTraceObserved)(const void* object,
                                                         ObjectKind kind);

}  

inline void TraceWait(const void* object, ObjectKind kind) {
  ABSL_INTERNAL_C_SYMBOL(AbslInternalTraceWait)(object, kind);
}

inline void TraceContinue(const void* object, ObjectKind kind) {
  ABSL_INTERNAL_C_SYMBOL(AbslInternalTraceContinue)(object, kind);
}

inline void TraceSignal(const void* object, ObjectKind kind) {
  ABSL_INTERNAL_C_SYMBOL(AbslInternalTraceSignal)(object, kind);
}

inline void TraceObserved(const void* object, ObjectKind kind) {
  ABSL_INTERNAL_C_SYMBOL(AbslInternalTraceObserved)(object, kind);
}

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_BASE_INTERNAL_TRACING_H_
