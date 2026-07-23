// Copyright 2018 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_CONTAINER_INTERNAL_HASHTABLE_DEBUG_HOOKS_H_
#define ABSL_CONTAINER_INTERNAL_HASHTABLE_DEBUG_HOOKS_H_

#include <cstddef>

#include <algorithm>
#include <type_traits>
#include <vector>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace container_internal {
namespace hashtable_debug_internal {

using std::get;
template <typename T, typename = typename T::mapped_type>
auto GetKey(const typename T::value_type& pair, int) -> decltype(get<0>(pair)) {
  return get<0>(pair);
}

template <typename T>
const typename T::key_type& GetKey(const typename T::key_type& key, char) {
  return key;
}

template <class Container, typename Enabler = void>
struct HashtableDebugAccess {
  static size_t GetNumProbes(const Container& c,
                             const typename Container::key_type& key) {
    if (!c.bucket_count()) return {};
    size_t num_probes = 0;
    size_t bucket = c.bucket(key);
    for (auto it = c.begin(bucket), e = c.end(bucket);; ++it, ++num_probes) {
      if (it == e) return num_probes;
      if (c.key_eq()(key, GetKey<Container>(*it, 0))) return num_probes;
    }
  }


};

}  
}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_CONTAINER_INTERNAL_HASHTABLE_DEBUG_HOOKS_H_
