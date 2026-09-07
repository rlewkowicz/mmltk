// Copyright 2022 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_LOG_LOG_SINK_REGISTRY_H_
#define ABSL_LOG_LOG_SINK_REGISTRY_H_

#include "absl/base/config.h"
#include "absl/base/nullability.h"
#include "absl/log/internal/log_sink_set.h"
#include "absl/log/log_sink.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

inline void AddLogSink(absl::LogSink* absl_nonnull sink) {
  log_internal::AddLogSink(sink);
}
inline void RemoveLogSink(absl::LogSink* absl_nonnull sink) {
  log_internal::RemoveLogSink(sink);
}

inline void FlushLogSinks() { log_internal::FlushLogSinks(); }

ABSL_NAMESPACE_END
}  

#endif  // ABSL_LOG_LOG_SINK_REGISTRY_H_
