// Copyright 2021 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_CLEANUP_CLEANUP_H_
#define ABSL_CLEANUP_CLEANUP_H_

#include <utility>

#include "absl/base/config.h"
#include "absl/base/internal/hardening.h"
#include "absl/base/macros.h"
#include "absl/cleanup/internal/cleanup.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

template <typename Arg, typename Callback = void()>
class [[nodiscard]] Cleanup final {
  static_assert(cleanup_internal::WasDeduced<Arg>(),
                "Explicit template parameters are not supported.");

  static_assert(cleanup_internal::ReturnsVoid<Callback>(),
                "Callbacks that return values are not supported.");

 public:
  Cleanup(Callback callback) : storage_(std::move(callback)) {}  // NOLINT

  Cleanup(Cleanup&& other) = default;

  void Cancel() && {
    absl::base_internal::HardeningAssert(storage_.IsCallbackEngaged());
    storage_.DestroyCallback();
  }

  void Invoke() && {
    absl::base_internal::HardeningAssert(storage_.IsCallbackEngaged());
    storage_.InvokeCallback();
    storage_.DestroyCallback();
  }

  ~Cleanup() {
    if (storage_.IsCallbackEngaged()) {
      storage_.InvokeCallback();
      storage_.DestroyCallback();
    }
  }

 private:
  cleanup_internal::Storage<Callback> storage_;
};

template <typename Callback>
Cleanup(Callback callback) -> Cleanup<cleanup_internal::Tag, Callback>;

template <typename... Args, typename Callback>
absl::Cleanup<cleanup_internal::Tag, Callback> MakeCleanup(Callback callback) {
  static_assert(cleanup_internal::WasDeduced<cleanup_internal::Tag, Args...>(),
                "Explicit template parameters are not supported.");

  static_assert(cleanup_internal::ReturnsVoid<Callback>(),
                "Callbacks that return values are not supported.");

  return {std::move(callback)};
}

ABSL_NAMESPACE_END
}  

#endif  // ABSL_CLEANUP_CLEANUP_H_
