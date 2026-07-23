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

#ifndef ABSL_CONTAINER_HASH_CONTAINER_DEFAULTS_H_
#define ABSL_CONTAINER_HASH_CONTAINER_DEFAULTS_H_

#include "absl/base/config.h"
#include "absl/container/internal/hash_function_defaults.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

template <typename T>
using DefaultHashContainerHash = absl::container_internal::hash_default_hash<T>;

template <typename T>
using DefaultHashContainerEq = absl::container_internal::hash_default_eq<T>;

ABSL_NAMESPACE_END
}  

#endif  // ABSL_CONTAINER_HASH_CONTAINER_DEFAULTS_H_
