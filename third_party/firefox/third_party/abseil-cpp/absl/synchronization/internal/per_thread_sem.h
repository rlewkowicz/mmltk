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


#ifndef ABSL_SYNCHRONIZATION_INTERNAL_PER_THREAD_SEM_H_
#define ABSL_SYNCHRONIZATION_INTERNAL_PER_THREAD_SEM_H_

#include <atomic>

#include "absl/base/internal/thread_identity.h"
#include "absl/synchronization/internal/create_thread_identity.h"
#include "absl/synchronization/internal/kernel_timeout.h"

namespace gloop_do_not_use {
struct SynchronizationBenchmarkPeer;
}  

namespace absl {
ABSL_NAMESPACE_BEGIN

class Mutex;

namespace synchronization_internal {

class PerThreadSem {
 public:
  PerThreadSem() = delete;
  PerThreadSem(const PerThreadSem&) = delete;
  PerThreadSem& operator=(const PerThreadSem&) = delete;

  static void Tick(base_internal::ThreadIdentity* identity);

  static void SetThreadBlockedCounter(std::atomic<int> *counter);
  static std::atomic<int> *GetThreadBlockedCounter();

 private:
  static inline void Init(base_internal::ThreadIdentity* identity);

  static inline void Post(base_internal::ThreadIdentity* identity);

  static inline bool Wait(KernelTimeout t);

  friend class PerThreadSemTest;
  friend class absl::Mutex;
  friend struct ::gloop_do_not_use::SynchronizationBenchmarkPeer;
  friend void OneTimeInitThreadIdentity(absl::base_internal::ThreadIdentity*);
};

}  
ABSL_NAMESPACE_END
}  

extern "C" {
void ABSL_INTERNAL_C_SYMBOL(AbslInternalPerThreadSemInit)(
    absl::base_internal::ThreadIdentity* identity);
void ABSL_INTERNAL_C_SYMBOL(AbslInternalPerThreadSemPost)(
    absl::base_internal::ThreadIdentity* identity);
bool ABSL_INTERNAL_C_SYMBOL(AbslInternalPerThreadSemWait)(
    absl::synchronization_internal::KernelTimeout t);
void ABSL_INTERNAL_C_SYMBOL(AbslInternalPerThreadSemPoke)(
    absl::base_internal::ThreadIdentity* identity);
}  

void absl::synchronization_internal::PerThreadSem::Init(
    absl::base_internal::ThreadIdentity* identity) {
  ABSL_INTERNAL_C_SYMBOL(AbslInternalPerThreadSemInit)(identity);
}

void absl::synchronization_internal::PerThreadSem::Post(
    absl::base_internal::ThreadIdentity* identity) {
  ABSL_INTERNAL_C_SYMBOL(AbslInternalPerThreadSemPost)(identity);
}

bool absl::synchronization_internal::PerThreadSem::Wait(
    absl::synchronization_internal::KernelTimeout t) {
  return ABSL_INTERNAL_C_SYMBOL(AbslInternalPerThreadSemWait)(t);
}

#endif  // ABSL_SYNCHRONIZATION_INTERNAL_PER_THREAD_SEM_H_
