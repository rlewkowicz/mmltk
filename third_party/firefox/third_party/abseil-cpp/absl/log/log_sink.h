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

#ifndef ABSL_LOG_LOG_SINK_H_
#define ABSL_LOG_LOG_SINK_H_

#include "absl/base/config.h"
#include "absl/log/log_entry.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

class LogSink {
 public:
  virtual ~LogSink() = default;

  virtual void Send(const absl::LogEntry& entry) = 0;

  virtual void Flush() {}

 protected:
  LogSink() = default;
  LogSink(const LogSink&) = default;
  LogSink& operator=(const LogSink&) = default;

 private:
  virtual void KeyFunction() const final;  // NOLINT(readability/inheritance)
};

ABSL_NAMESPACE_END
}  

#endif  // ABSL_LOG_LOG_SINK_H_
