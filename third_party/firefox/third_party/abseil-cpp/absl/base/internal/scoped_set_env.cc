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

#include "absl/base/internal/scoped_set_env.h"


#include <cstdlib>

#include "absl/base/internal/raw_logging.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace base_internal {

namespace {


void SetEnvVar(const char* name, const char* value) {
  if (value == nullptr) {
    ::unsetenv(name);
  } else {
    ::setenv(name, value, 1);
  }
}

}  

ScopedSetEnv::ScopedSetEnv(const char* var_name, const char* new_value)
    : var_name_(var_name), was_unset_(false) {
  const char* val = ::getenv(var_name_.c_str());
  if (val == nullptr) {
    was_unset_ = true;
  } else {
    old_value_ = val;
  }

  SetEnvVar(var_name_.c_str(), new_value);
}

ScopedSetEnv::~ScopedSetEnv() {
  SetEnvVar(var_name_.c_str(), was_unset_ ? nullptr : old_value_.c_str());
}

}  
ABSL_NAMESPACE_END
}  
