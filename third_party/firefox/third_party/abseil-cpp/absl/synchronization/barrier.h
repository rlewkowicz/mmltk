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

#ifndef ABSL_SYNCHRONIZATION_BARRIER_H_
#define ABSL_SYNCHRONIZATION_BARRIER_H_

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

class Barrier {
 public:
  explicit Barrier(int num_threads)
      : num_to_block_(num_threads), num_to_exit_(num_threads) {}

  Barrier(const Barrier&) = delete;
  Barrier& operator=(const Barrier&) = delete;

  bool Block();

 private:
  Mutex lock_;
  int num_to_block_ ABSL_GUARDED_BY(lock_);
  int num_to_exit_ ABSL_GUARDED_BY(lock_);
};

ABSL_NAMESPACE_END
}  
#endif  // ABSL_SYNCHRONIZATION_BARRIER_H_
