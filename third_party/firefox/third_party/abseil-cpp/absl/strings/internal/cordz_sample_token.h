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

#include "absl/base/config.h"
#include "absl/strings/internal/cordz_handle.h"
#include "absl/strings/internal/cordz_info.h"

#ifndef ABSL_STRINGS_INTERNAL_CORDZ_SAMPLE_TOKEN_H_
#define ABSL_STRINGS_INTERNAL_CORDZ_SAMPLE_TOKEN_H_

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace cord_internal {

class CordzSampleToken : public CordzSnapshot {
 public:
  class Iterator {
   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = const CordzInfo&;
    using difference_type = ptrdiff_t;
    using pointer = const CordzInfo*;
    using reference = value_type;

    Iterator() = default;

    Iterator& operator++();
    Iterator operator++(int);
    friend bool operator==(const Iterator& lhs, const Iterator& rhs);
    friend bool operator!=(const Iterator& lhs, const Iterator& rhs);
    reference operator*() const;
    pointer operator->() const;

   private:
    friend class CordzSampleToken;
    explicit Iterator(const CordzSampleToken* token);

    const CordzSampleToken* token_ = nullptr;
    pointer current_ = nullptr;
  };

  CordzSampleToken() = default;
  CordzSampleToken(const CordzSampleToken&) = delete;
  CordzSampleToken& operator=(const CordzSampleToken&) = delete;

  Iterator begin() { return Iterator(this); }
  Iterator end() { return Iterator(); }
};

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_INTERNAL_CORDZ_SAMPLE_TOKEN_H_
