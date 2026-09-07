// Copyright 2022 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_LOG_DIE_IF_NULL_H_
#define ABSL_LOG_DIE_IF_NULL_H_

#include <stdint.h>

#include <type_traits>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/internal/nullability_traits.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"

#define ABSL_DIE_IF_NULL(val) \
  ::absl::log_internal::DieIfNull(__FILE__, __LINE__, #val, (val))

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace log_internal {

[[noreturn]] ABSL_ATTRIBUTE_NOINLINE void DieBecauseNull(
    const char* absl_nonnull file, int line, const char* absl_nonnull exprtext);


template <typename T>
[[nodiscard]] typename absl::base_internal::AddNonnullIfCompatible<
    std::remove_reference_t<T>>::type&
DieIfNull(const char* absl_nonnull file, int line,
          const char* absl_nonnull exprtext, T& t) {
  if (ABSL_PREDICT_FALSE(t == nullptr)) {
    DieBecauseNull(file, line, exprtext);
  }
  return t;
}

template <typename T>
[[nodiscard]] typename absl::base_internal::AddNonnullIfCompatible<
    std::remove_reference_t<T>>::type&&
DieIfNull(const char* absl_nonnull file, int line,
          const char* absl_nonnull exprtext, T&& t) {
  if (ABSL_PREDICT_FALSE(t == nullptr)) {
    DieBecauseNull(file, line, exprtext);
  }
  return std::forward<T>(t);
}

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_LOG_DIE_IF_NULL_H_
