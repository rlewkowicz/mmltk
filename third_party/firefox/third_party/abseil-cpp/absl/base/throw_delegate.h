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

#ifndef ABSL_BASE_THROW_DELEGATE_H_
#define ABSL_BASE_THROW_DELEGATE_H_

#include <string>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN


[[noreturn]] void ThrowStdLogicError(const std::string& what_arg);
[[noreturn]] void ThrowStdLogicError(const char* what_arg);
[[noreturn]] void ThrowStdInvalidArgument(const std::string& what_arg);
[[noreturn]] void ThrowStdInvalidArgument(const char* what_arg);
[[noreturn]] void ThrowStdDomainError(const std::string& what_arg);
[[noreturn]] void ThrowStdDomainError(const char* what_arg);
[[noreturn]] void ThrowStdLengthError(const std::string& what_arg);
[[noreturn]] void ThrowStdLengthError(const char* what_arg);
[[noreturn]] void ThrowStdOutOfRange(const std::string& what_arg);
[[noreturn]] void ThrowStdOutOfRange(const char* what_arg);
[[noreturn]] void ThrowStdRuntimeError(const std::string& what_arg);
[[noreturn]] void ThrowStdRuntimeError(const char* what_arg);
[[noreturn]] void ThrowStdRangeError(const std::string& what_arg);
[[noreturn]] void ThrowStdRangeError(const char* what_arg);
[[noreturn]] void ThrowStdOverflowError(const std::string& what_arg);
[[noreturn]] void ThrowStdOverflowError(const char* what_arg);
[[noreturn]] void ThrowStdUnderflowError(const std::string& what_arg);
[[noreturn]] void ThrowStdUnderflowError(const char* what_arg);

[[noreturn]] void ThrowStdBadFunctionCall();
[[noreturn]] void ThrowStdBadAlloc();
[[noreturn]] void ThrowStdBadArrayNewLength();

ABSL_NAMESPACE_END
}  

#endif  // ABSL_BASE_THROW_DELEGATE_H_
