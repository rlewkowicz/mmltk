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

#ifndef ABSL_LOG_INTERNAL_LOG_SINK_SET_H_
#define ABSL_LOG_INTERNAL_LOG_SINK_SET_H_

#include "absl/base/config.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/types/span.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace log_internal {

bool ThreadIsLoggingToLogSink();

void LogToSinks(const absl::LogEntry& entry,
                absl::Span<absl::LogSink*> extra_sinks, bool extra_sinks_only);

void AddLogSink(absl::LogSink* sink);
void RemoveLogSink(absl::LogSink* sink);
void FlushLogSinks();

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_LOG_INTERNAL_LOG_SINK_SET_H_
