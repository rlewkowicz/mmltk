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

#ifndef ABSL_LOG_INTERNAL_GLOBALS_H_
#define ABSL_LOG_INTERNAL_GLOBALS_H_

#include "absl/base/config.h"
#include "absl/base/log_severity.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace log_internal {

bool IsInitialized();

void SetInitialized();

void WriteToStderr(absl::string_view message, absl::LogSeverity severity);

void SetTimeZone(absl::TimeZone tz);

const absl::TimeZone* TimeZone();

bool ShouldSymbolizeLogStackTrace();

void EnableSymbolizeLogStackTrace(bool on_off);

int MaxFramesInLogStackTrace();

void SetMaxFramesInLogStackTrace(int max_num_frames);

bool ExitOnDFatal();

void SetExitOnDFatal(bool on_off);

bool SuppressSigabortTrace();

bool SetSuppressSigabortTrace(bool on_off);

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_LOG_INTERNAL_GLOBALS_H_
