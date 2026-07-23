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

#ifndef ABSL_FLAGS_INTERNAL_REGISTRY_H_
#define ABSL_FLAGS_INTERNAL_REGISTRY_H_

#include <functional>

#include "absl/base/config.h"
#include "absl/base/fast_type_id.h"
#include "absl/flags/commandlineflag.h"
#include "absl/flags/internal/commandlineflag.h"
#include "absl/strings/string_view.h"


namespace absl {
ABSL_NAMESPACE_BEGIN
namespace flags_internal {

void ForEachFlag(std::function<void(CommandLineFlag&)> visitor);


bool RegisterCommandLineFlag(CommandLineFlag&, const char* filename);

void FinalizeRegistry();


void Retire(const char* name, FlagFastTypeId type_id, unsigned char* buf);

constexpr size_t kRetiredFlagObjSize = 3 * sizeof(void*);
constexpr size_t kRetiredFlagObjAlignment = alignof(void*);

template <typename T>
class RetiredFlag {
 public:
  void Retire(const char* flag_name) {
    flags_internal::Retire(flag_name, absl::FastTypeId<T>(), buf_);
  }

 private:
  alignas(kRetiredFlagObjAlignment) unsigned char buf_[kRetiredFlagObjSize];
};

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_FLAGS_INTERNAL_REGISTRY_H_
