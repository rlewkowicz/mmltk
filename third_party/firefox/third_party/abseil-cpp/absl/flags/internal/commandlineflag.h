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

#ifndef ABSL_FLAGS_INTERNAL_COMMANDLINEFLAG_H_
#define ABSL_FLAGS_INTERNAL_COMMANDLINEFLAG_H_

#include "absl/base/config.h"
#include "absl/base/fast_type_id.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace flags_internal {

using FlagFastTypeId = absl::FastTypeIdType;

enum FlagSettingMode {
  SET_FLAGS_VALUE,
  SET_FLAG_IF_DEFAULT,
  SET_FLAGS_DEFAULT
};

enum ValueSource {
  kCommandLine,
  kProgrammaticChange,
};

class FlagStateInterface {
 public:
  virtual ~FlagStateInterface();

  virtual void Restore() && = 0;
};

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_FLAGS_INTERNAL_COMMANDLINEFLAG_H_
