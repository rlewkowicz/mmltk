//  Copyright 2019 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_FLAGS_DECLARE_H_
#define ABSL_FLAGS_DECLARE_H_

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace flags_internal {

template <typename T>
class Flag;

}  

template <typename T>
using Flag = flags_internal::Flag<T>;

ABSL_NAMESPACE_END
}  

#define ABSL_DECLARE_FLAG(type, name) ABSL_DECLARE_FLAG_INTERNAL(type, name)

#if defined(_MSC_VER)
#define ABSL_DECLARE_FLAG_INTERNAL(type, name) \
  extern absl::Flag<type> FLAGS_##name
#else
#define ABSL_DECLARE_FLAG_INTERNAL(type, name)               \
  extern absl::Flag<type> FLAGS_##name;                      \
  namespace absl  {}          \
   \
  extern absl::Flag<type> FLAGS_##name
#endif  // _MSC_VER

#endif  // ABSL_FLAGS_DECLARE_H_
