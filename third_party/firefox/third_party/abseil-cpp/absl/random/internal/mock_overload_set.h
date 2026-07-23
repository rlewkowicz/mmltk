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

#ifndef ABSL_RANDOM_INTERNAL_MOCK_OVERLOAD_SET_H_
#define ABSL_RANDOM_INTERNAL_MOCK_OVERLOAD_SET_H_

#include <tuple>
#include <type_traits>

#include "gmock/gmock.h"
#include "absl/base/config.h"
#include "absl/random/internal/mock_helpers.h"
#include "absl/random/mocking_bit_gen.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace random_internal {

template <typename DistrT, typename ValidatorT, typename Fn>
struct MockSingleOverload;

template <typename DistrT, typename ValidatorT, typename Ret, typename... Args>
struct MockSingleOverload<DistrT, ValidatorT, Ret(MockingBitGen&, Args...)> {
  static_assert(std::is_same_v<typename DistrT::result_type, Ret>,
                "Overload signature must have return type matching the "
                "distribution result_type.");
  using KeyT = Ret(DistrT, std::tuple<Args...>);

  template <typename MockURBG>
  auto gmock_Call(MockURBG& gen, const ::testing::Matcher<Args>&... matchers)
      -> decltype(MockHelpers::MockFor<KeyT>(gen, ValidatorT())
                      .gmock_Call(matchers...)) {
    static_assert(std::is_base_of_v<MockingBitGen, MockURBG>,
                  "Mocking requires an absl::MockingBitGen");
    return MockHelpers::MockFor<KeyT>(gen, ValidatorT())
        .gmock_Call(matchers...);
  }
};

template <typename DistrT, typename ValidatorT, typename Ret, typename Arg,
          typename... Args>
struct MockSingleOverload<DistrT, ValidatorT,
                          Ret(Arg, MockingBitGen&, Args...)> {
  static_assert(std::is_same_v<typename DistrT::result_type, Ret>,
                "Overload signature must have return type matching the "
                "distribution result_type.");
  using KeyT = Ret(DistrT, std::tuple<Arg, Args...>);

  template <typename MockURBG>
  auto gmock_Call(const ::testing::Matcher<Arg>& matcher, MockURBG& gen,
                  const ::testing::Matcher<Args>&... matchers)
      -> decltype(MockHelpers::MockFor<KeyT>(gen, ValidatorT())
                      .gmock_Call(matcher, matchers...)) {
    static_assert(std::is_base_of_v<MockingBitGen, MockURBG>,
                  "Mocking requires an absl::MockingBitGen");
    return MockHelpers::MockFor<KeyT>(gen, ValidatorT())
        .gmock_Call(matcher, matchers...);
  }
};

template <typename DistrT, typename ValidatorT, typename... Fns>
struct MockOverloadSetWithValidator;

template <typename DistrT, typename ValidatorT, typename Sig>
struct MockOverloadSetWithValidator<DistrT, ValidatorT, Sig>
    : public MockSingleOverload<DistrT, ValidatorT, Sig> {
  using MockSingleOverload<DistrT, ValidatorT, Sig>::gmock_Call;
};

template <typename DistrT, typename ValidatorT, typename FirstSig,
          typename... Rest>
struct MockOverloadSetWithValidator<DistrT, ValidatorT, FirstSig, Rest...>
    : public MockSingleOverload<DistrT, ValidatorT, FirstSig>,
      public MockOverloadSetWithValidator<DistrT, ValidatorT, Rest...> {
  using MockSingleOverload<DistrT, ValidatorT, FirstSig>::gmock_Call;
  using MockOverloadSetWithValidator<DistrT, ValidatorT, Rest...>::gmock_Call;
};

template <typename DistrT, typename... Signatures>
using MockOverloadSet =
    MockOverloadSetWithValidator<DistrT, NoOpValidator, Signatures...>;

}  
ABSL_NAMESPACE_END
}  
#endif  // ABSL_RANDOM_INTERNAL_MOCK_OVERLOAD_SET_H_
