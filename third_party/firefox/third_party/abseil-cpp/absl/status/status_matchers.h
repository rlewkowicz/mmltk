// Copyright 2024 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_STATUS_STATUS_MATCHERS_H_
#define ABSL_STATUS_STATUS_MATCHERS_H_

#include <ostream>  // NOLINT
#include <type_traits>
#include <utility>

#include "gmock/gmock.h"  // gmock_for_status_matchers.h
#include "absl/base/config.h"
#include "absl/status/internal/status_matchers.h"

namespace absl_testing {
ABSL_NAMESPACE_BEGIN

#define ABSL_EXPECT_OK(expression) \
  EXPECT_THAT(expression, ::absl_testing::IsOk())
#define ABSL_ASSERT_OK(expression) \
  ASSERT_THAT(expression, ::absl_testing::IsOk())

template <typename InnerMatcherT>
status_internal::IsOkAndHoldsMatcher<std::decay_t<InnerMatcherT>> IsOkAndHolds(
    InnerMatcherT&& inner_matcher) {
  return status_internal::IsOkAndHoldsMatcher<std::decay_t<InnerMatcherT>>(
      std::forward<InnerMatcherT>(inner_matcher));
}

template <typename StatusCodeMatcherT, typename StatusMessageMatcherT>
status_internal::StatusIsMatcher StatusIs(
    StatusCodeMatcherT&& code_matcher,
    StatusMessageMatcherT&& message_matcher) {
  return status_internal::StatusIsMatcher(
      std::forward<StatusCodeMatcherT>(code_matcher),
      std::forward<StatusMessageMatcherT>(message_matcher));
}

template <typename StatusCodeMatcherT>
status_internal::StatusIsMatcher StatusIs(StatusCodeMatcherT&& code_matcher) {
  return absl_testing::StatusIs(std::forward<StatusCodeMatcherT>(code_matcher),
                                ::testing::_);
}

inline status_internal::IsOkMatcher IsOk() {
  return status_internal::IsOkMatcher();
}

#ifdef ABSL_DEFINE_UNQUALIFIED_STATUS_TESTING_MACROS
#define EXPECT_OK(expression) ABSL_EXPECT_OK(expression)
#define ASSERT_OK(expression) ABSL_ASSERT_OK(expression)
#endif  // ABSL_DEFINE_UNQUALIFIED_STATUS_TESTING_MACROS

ABSL_NAMESPACE_END
}  

#endif  // ABSL_STATUS_STATUS_MATCHERS_H_
