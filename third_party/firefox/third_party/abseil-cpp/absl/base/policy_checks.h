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


#if !defined(ABSL_BASE_POLICY_CHECKS_H_)
#define ABSL_BASE_POLICY_CHECKS_H_

#include <limits.h>

#if defined(__cplusplus)
#include <cstddef>
#endif




#if defined(_MSC_VER) && _MSC_VER < 1930 && !defined(__clang__)
#error "This package requires Visual Studio 2022 (MSVC++ 17.0) or higher."
#endif

#if defined(__GNUC__) && !defined(__clang__)
#if __GNUC__ < 10
#error "This package requires GCC 10 or higher."
#endif
#endif

#if defined(__apple_build_version__) && __apple_build_version__ < 4211165
#error "This package requires __apple_build_version__ of 4211165 or higher."
#endif


#if defined(_MSVC_LANG)
#if _MSVC_LANG < 201703L
#error "C++ versions less than C++17 are not supported."
#endif
#elif defined(__cplusplus)
#if __cplusplus < 201703L
#error "C++ versions less than C++17 are not supported."
#endif
#endif


#if defined(_STLPORT_VERSION)
#error "STLPort is not supported."
#endif


#if CHAR_BIT != 8
#error "Abseil assumes CHAR_BIT == 8."
#endif


#if INT_MAX < 2147483647
#error "Abseil assumes that int is at least 4 bytes. "
#endif

#endif
