// Copyright 2023 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_SYNCHRONIZATION_INTERNAL_WAITER_BASE_H_
#define ABSL_SYNCHRONIZATION_INTERNAL_WAITER_BASE_H_

#include "absl/base/config.h"
#include "absl/base/internal/thread_identity.h"
#include "absl/synchronization/internal/kernel_timeout.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace synchronization_internal {

class WaiterBase {
 public:
  WaiterBase() = default;

  WaiterBase(const WaiterBase&) = delete;
  WaiterBase& operator=(const WaiterBase&) = delete;





#ifndef ABSL_HAVE_THREAD_SANITIZER
  static constexpr int kIdlePeriods = 60;
#else
  static constexpr int kIdlePeriods = 1;
#endif

 protected:
  static void MaybeBecomeIdle();
};

template <typename T>
class WaiterCrtp : public WaiterBase {
 public:
  static T* GetWaiter(base_internal::ThreadIdentity* identity) {
    static_assert(
        sizeof(T) <= sizeof(base_internal::ThreadIdentity::WaiterState),
        "Insufficient space for Waiter");
    return reinterpret_cast<T*>(identity->waiter_state.data);
  }
};

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_SYNCHRONIZATION_INTERNAL_WAITER_BASE_H_
