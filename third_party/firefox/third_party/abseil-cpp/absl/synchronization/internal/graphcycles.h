// Copyright 2017 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_SYNCHRONIZATION_INTERNAL_GRAPHCYCLES_H_
#define ABSL_SYNCHRONIZATION_INTERNAL_GRAPHCYCLES_H_



#include <cstdint>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace synchronization_internal {

struct GraphId {
  uint64_t handle;

  bool operator==(const GraphId& x) const { return handle == x.handle; }
  bool operator!=(const GraphId& x) const { return handle != x.handle; }
};

inline GraphId InvalidGraphId() {
  return GraphId{0};
}

class GraphCycles {
 public:
  GraphCycles();
  ~GraphCycles();

  GraphId GetId(void* ptr);

  void RemoveNode(void* ptr);

  void* Ptr(GraphId id);

  bool InsertEdge(GraphId source_node, GraphId dest_node);

  void RemoveEdge(GraphId source_node, GraphId dest_node);

  bool HasNode(GraphId node);

  bool HasEdge(GraphId source_node, GraphId dest_node) const;

  bool IsReachable(GraphId source_node, GraphId dest_node) const;

  int FindPath(GraphId source, GraphId dest, int max_path_len,
               GraphId path[]) const;

  void UpdateStackTrace(GraphId id, int priority,
                        int (*get_stack_trace)(void**, int));

  int GetStackTrace(GraphId id, void*** ptr);

  bool CheckInvariants() const;

  void TestOnlyAddNodes(uint32_t n);

  struct Rep;
 private:
  Rep *rep_;      
  GraphCycles(const GraphCycles&) = delete;
  GraphCycles& operator=(const GraphCycles&) = delete;
};

}  
ABSL_NAMESPACE_END
}  

#endif
