// Copyright 2020 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_FLAGS_REFLECTION_H_
#define ABSL_FLAGS_REFLECTION_H_

#include <string>

#include "absl/base/config.h"
#include "absl/container/flat_hash_map.h"
#include "absl/flags/commandlineflag.h"
#include "absl/flags/internal/commandlineflag.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace flags_internal {
class FlagSaverImpl;
}  

absl::CommandLineFlag* FindCommandLineFlag(absl::string_view name);

absl::flat_hash_map<absl::string_view, absl::CommandLineFlag*> GetAllFlags();


class FlagSaver {
 public:
  FlagSaver();
  ~FlagSaver();

  FlagSaver(const FlagSaver&) = delete;
  void operator=(const FlagSaver&) = delete;

 private:
  flags_internal::FlagSaverImpl* impl_;
};


ABSL_NAMESPACE_END
}  

#endif  // ABSL_FLAGS_REFLECTION_H_
