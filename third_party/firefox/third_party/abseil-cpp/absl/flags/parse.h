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

#ifndef ABSL_FLAGS_PARSE_H_
#define ABSL_FLAGS_PARSE_H_

#include <string>
#include <vector>

#include "absl/base/config.h"
#include "absl/flags/internal/parse.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

struct UnrecognizedFlag {
  enum Source { kFromArgv, kFromFlagfile };

  explicit UnrecognizedFlag(Source s, absl::string_view f)
      : source(s), flag_name(f) {}
  Source source;
  std::string flag_name;
};

inline bool operator==(const UnrecognizedFlag& lhs,
                       const UnrecognizedFlag& rhs) {
  return lhs.source == rhs.source && lhs.flag_name == rhs.flag_name;
}

namespace flags_internal {

HelpMode ParseAbseilFlagsOnlyImpl(
    int argc, char* argv[], std::vector<char*>& positional_args,
    std::vector<UnrecognizedFlag>& unrecognized_flags,
    UsageFlagsAction usage_flag_action);

}  

void ParseAbseilFlagsOnly(int argc, char* argv[],
                          std::vector<char*>& positional_args,
                          std::vector<UnrecognizedFlag>& unrecognized_flags);

void ReportUnrecognizedFlags(
    const std::vector<UnrecognizedFlag>& unrecognized_flags);

std::vector<char*> ParseCommandLine(int argc, char* argv[]);

ABSL_NAMESPACE_END
}  

#endif  // ABSL_FLAGS_PARSE_H_
