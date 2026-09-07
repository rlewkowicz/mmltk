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

#ifndef ABSL_LOG_ABSL_VLOG_IS_ON_H_
#define ABSL_LOG_ABSL_VLOG_IS_ON_H_

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/log/internal/vlog_config.h"  // IWYU pragma: export
#include "absl/strings/string_view.h"

#ifdef ABSL_MAX_VLOG_VERBOSITY
#define ABSL_LOG_INTERNAL_MAX_LOG_VERBOSITY_CHECK(x) \
  ((x) <= ABSL_MAX_VLOG_VERBOSITY)&&
#else
#define ABSL_LOG_INTERNAL_MAX_LOG_VERBOSITY_CHECK(x)
#endif

#define ABSL_VLOG_IS_ON(verbose_level)                                     \
  (ABSL_LOG_INTERNAL_MAX_LOG_VERBOSITY_CHECK(verbose_level)[]()            \
       ->::absl::log_internal::VLogSite *                                  \
   {                                                                       \
     ABSL_CONST_INIT static ::absl::log_internal::VLogSite site(__FILE__); \
     return &site;                                                         \
   }()                                                                     \
       ->IsEnabled(verbose_level))

#endif  // ABSL_LOG_ABSL_VLOG_IS_ON_H_
