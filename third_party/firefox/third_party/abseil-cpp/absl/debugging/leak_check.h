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
#ifndef ABSL_DEBUGGING_LEAK_CHECK_H_
#define ABSL_DEBUGGING_LEAK_CHECK_H_

#include <cstddef>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

bool HaveLeakSanitizer();

bool LeakCheckerIsActive();

void DoIgnoreLeak(const void* ptr);

template <typename T>
T* IgnoreLeak(T* ptr) {
  DoIgnoreLeak(ptr);
  return ptr;
}

bool FindAndReportLeaks();

class LeakCheckDisabler {
 public:
  LeakCheckDisabler();
  LeakCheckDisabler(const LeakCheckDisabler&) = delete;
  LeakCheckDisabler& operator=(const LeakCheckDisabler&) = delete;
  ~LeakCheckDisabler();
};

void RegisterLivePointers(const void* ptr, size_t size);

void UnRegisterLivePointers(const void* ptr, size_t size);

ABSL_NAMESPACE_END
}  

#endif  // ABSL_DEBUGGING_LEAK_CHECK_H_
