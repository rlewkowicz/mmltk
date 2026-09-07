// Copyright 2019 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_RANDOM_MOCKING_ACCESS_H_
#define ABSL_RANDOM_MOCKING_ACCESS_H_

#include <type_traits>
#include <utility>

#include "absl/base/config.h"
#include "absl/base/fast_type_id.h"
#include "absl/meta/type_traits.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

class RandomMockingAccess {
  template <template <class...> class Trait, class AlwaysVoid, class... Args>
  struct detector : std::false_type {};
  template <template <class...> class Trait, class... Args>
  struct detector<Trait, std::void_t<Trait<Args...>>, Args...>
      : std::true_type {};

  using IdType = ::absl::FastTypeIdType;

  template <class T>
  using invoke_mock_t = decltype(std::declval<T*>()->InvokeMock(
      std::declval<IdType>(), std::declval<void*>(), std::declval<void*>()));

 public:
  template <typename T>
  using HasInvokeMock = typename detector<invoke_mock_t, void, T>::type;

  template <typename URBG>
  static inline bool InvokeMock(URBG* urbg, IdType key_id, void* args_tuple,
                                void* result) {
    return urbg->InvokeMock(key_id, args_tuple, result);
  }
};

ABSL_NAMESPACE_END
}  

#endif  // ABSL_RANDOM_MOCKING_ACCESS_H_
