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
#ifndef ABSL_FLAGS_USAGE_CONFIG_H_
#define ABSL_FLAGS_USAGE_CONFIG_H_

#include <functional>
#include <string>

#include "absl/base/config.h"
#include "absl/strings/string_view.h"


namespace absl {
ABSL_NAMESPACE_BEGIN

namespace flags_internal {
using FlagKindFilter = std::function<bool (absl::string_view)>;
}  

struct FlagsUsageConfig {
  flags_internal::FlagKindFilter contains_helpshort_flags;

  flags_internal::FlagKindFilter contains_help_flags;

  flags_internal::FlagKindFilter contains_helppackage_flags;

  std::function<std::string()> version_string;

  std::function<std::string(absl::string_view)> normalize_filename;
};

void SetFlagsUsageConfig(FlagsUsageConfig usage_config);

namespace flags_internal {

FlagsUsageConfig GetUsageConfig();

void ReportUsageError(absl::string_view msg, bool is_fatal);

}  
ABSL_NAMESPACE_END
}  

extern "C" {

void ABSL_INTERNAL_C_SYMBOL(AbslInternalReportFatalUsageError)(
    absl::string_view);

}  

#endif  // ABSL_FLAGS_USAGE_CONFIG_H_
