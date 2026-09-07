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

#ifndef ABSL_FLAGS_INTERNAL_USAGE_H_
#define ABSL_FLAGS_INTERNAL_USAGE_H_

#include <iosfwd>
#include <ostream>
#include <string>

#include "absl/base/config.h"
#include "absl/flags/commandlineflag.h"
#include "absl/strings/string_view.h"


namespace absl {
ABSL_NAMESPACE_BEGIN
namespace flags_internal {

enum class HelpFormat {
  kHumanReadable,
};

enum class HelpMode {
  kNone,
  kImportant,
  kShort,
  kFull,
  kPackage,
  kMatch,
  kVersion,
  kOnlyCheckArgs
};

void FlagHelp(std::ostream& out, const CommandLineFlag& flag,
              HelpFormat format = HelpFormat::kHumanReadable);

void FlagsHelp(std::ostream& out, absl::string_view filter,
               HelpFormat format, absl::string_view program_usage_message);


HelpMode HandleUsageFlags(std::ostream& out,
                          absl::string_view program_usage_message);


void MaybeExit(HelpMode mode);


std::string GetFlagsHelpMatchSubstr();
HelpMode GetFlagsHelpMode();
HelpFormat GetFlagsHelpFormat();

void SetFlagsHelpMatchSubstr(absl::string_view);
void SetFlagsHelpMode(HelpMode);
void SetFlagsHelpFormat(HelpFormat);

bool DeduceUsageFlags(absl::string_view name, absl::string_view value);

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_FLAGS_INTERNAL_USAGE_H_
