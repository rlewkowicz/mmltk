// Copyright 2022 The Abseil Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_BASE_INTERNAL_CPU_DETECT_H_
#define ABSL_BASE_INTERNAL_CPU_DETECT_H_

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace base_internal {

enum class CpuType {
  kUnknown,
  kIntelHaswell,
  kAmdRome,
  kAmdNaples,
  kAmdMilan,
  kAmdGenoa,
  kAmdTurin,
  kAmdRyzenV3000,
  kIntelCascadelakeXeon,
  kIntelSkylakeXeon,
  kIntelBroadwell,
  kIntelIcelake,
  kIntelSapphirerapids,
  kIntelEmeraldrapids,
  kIntelGraniterapids,
  kIntelSkylake,
  kIntelIvybridge,
  kIntelSandybridge,
  kIntelWestmere,
  kArmNeoverseN1,
  kArmNeoverseV1,
  kAmpereSiryn,
  kArmNeoverseN2,
  kArmNeoverseV2,
  kArmNeoverseN3,
  kNvidiaGrace,
};

CpuType GetCpuType();

bool SupportsArmCRC32PMULL();

bool SupportsBmi2();

bool IsSMTEnabled();

int NumContextsPerCPU();

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_BASE_INTERNAL_CPU_DETECT_H_
