// Copyright 2017 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_SYNCHRONIZATION_BLOCKING_COUNTER_H_
#define ABSL_SYNCHRONIZATION_BLOCKING_COUNTER_H_

#include <atomic>

#include "absl/base/internal/tracing.h"
#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

class BlockingCounter {
 public:
  explicit BlockingCounter(int initial_count);

  BlockingCounter(const BlockingCounter&) = delete;
  BlockingCounter& operator=(const BlockingCounter&) = delete;

  bool DecrementCount();

  void Wait();

 private:
  static inline constexpr base_internal::ObjectKind TraceObjectKind() {
    return base_internal::ObjectKind::kBlockingCounter;
  }

  Mutex lock_;
  std::atomic<int> count_;
  int num_waiting_ ABSL_GUARDED_BY(lock_);
  bool done_ ABSL_GUARDED_BY(lock_);
};

ABSL_NAMESPACE_END
}  

#endif  // ABSL_SYNCHRONIZATION_BLOCKING_COUNTER_H_
