// Copyright 2018 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_DEBUGGING_STACKTRACE_H_
#define ABSL_DEBUGGING_STACKTRACE_H_

#include <stddef.h>
#include <stdint.h>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

namespace internal_stacktrace {

extern int GetStackTraceNoFixup(void** result, int max_depth, int skip_count);

}  

extern int GetStackFrames(void** result, int* sizes, int max_depth,
                          int skip_count);

extern int GetStackFramesWithContext(void** result, int* sizes, int max_depth,
                                     int skip_count, const void* uc,
                                     int* min_dropped_frames);

extern int GetStackTrace(void** result, int max_depth, int skip_count);

extern int GetStackTraceWithContext(void** result, int max_depth,
                                    int skip_count, const void* uc,
                                    int* min_dropped_frames);

extern void SetStackUnwinder(int (*unwinder)(void** pcs, int* sizes,
                                             int max_depth, int skip_count,
                                             const void* uc,
                                             int* min_dropped_frames));

extern int DefaultStackUnwinder(void** pcs, int* sizes, int max_depth,
                                int skip_count, const void* uc,
                                int* min_dropped_frames);

namespace debugging_internal {
extern bool StackTraceWorksForTest();
}  

namespace internal_stacktrace {
extern bool ShouldFixUpStack();

extern void FixUpStack(void** pcs, uintptr_t* frames, int* sizes,
                       size_t capacity, size_t& depth);
}  

ABSL_NAMESPACE_END
}  

#endif  // ABSL_DEBUGGING_STACKTRACE_H_
