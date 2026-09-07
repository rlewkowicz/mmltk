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


#if !defined(ABSL_DEBUGGING_INTERNAL_STACK_CONSUMPTION_H_)
#define ABSL_DEBUGGING_INTERNAL_STACK_CONSUMPTION_H_

#include "absl/base/config.h"

#if defined(ABSL_INTERNAL_HAVE_DEBUGGING_STACK_CONSUMPTION)
#error ABSL_INTERNAL_HAVE_DEBUGGING_STACK_CONSUMPTION cannot be set directly
#elif !0 && !0 && !defined(__Fuchsia__) && \
    (defined(__i386__) || defined(__x86_64__) || defined(__ppc__) || \
     defined(__aarch64__) || defined(__riscv))
#define ABSL_INTERNAL_HAVE_DEBUGGING_STACK_CONSUMPTION 1

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {

int GetSignalHandlerStackConsumption(void (*signal_handler)(int));

}  
ABSL_NAMESPACE_END
}  

#endif

#endif
