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

#ifndef ABSL_STRINGS_INTERNAL_CORDZ_STATISTICS_H_
#define ABSL_STRINGS_INTERNAL_CORDZ_STATISTICS_H_

#include <cstdint>

#include "absl/base/config.h"
#include "absl/strings/internal/cordz_update_tracker.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace cord_internal {

struct CordzStatistics {
  using MethodIdentifier = CordzUpdateTracker::MethodIdentifier;

  struct NodeCounts {
    size_t flat = 0;       
    size_t flat_64 = 0;    
    size_t flat_128 = 0;   
    size_t flat_256 = 0;   
    size_t flat_512 = 0;   
    size_t flat_1k = 0;    
    size_t external = 0;   
    size_t substring = 0;  
    size_t concat = 0;     
    size_t ring = 0;       
    size_t btree = 0;      
    size_t crc = 0;        
  };

  size_t size = 0;

  size_t estimated_memory_usage = 0;

  size_t estimated_fair_share_memory_usage = 0;

  size_t node_count = 0;

  NodeCounts node_counts;

  MethodIdentifier method = MethodIdentifier::kUnknown;

  MethodIdentifier parent_method = MethodIdentifier::kUnknown;

  CordzUpdateTracker update_tracker;
};

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_INTERNAL_CORDZ_STATISTICS_H_
