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

#ifndef ABSL_LOG_LOG_ENTRY_H_
#define ABSL_LOG_LOG_ENTRY_H_

#include <cstddef>
#include <ostream>
#include <string>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/log_severity.h"
#include "absl/log/internal/config.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

namespace log_internal {
class LogEntryTestPeer;
class LogMessage;
}  

class LogEntry final {
 public:
  using tid_t = log_internal::Tid;

  static constexpr int kNoVerbosityLevel = -1;
  static constexpr int kNoVerboseLevel = -1;  

  LogEntry(const LogEntry&) = delete;
  LogEntry& operator=(const LogEntry&) = delete;

  absl::string_view source_filename() const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return full_filename_;
  }
  absl::string_view source_basename() const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return base_filename_;
  }
  int source_line() const { return line_; }

  bool prefix() const { return prefix_; }

  absl::LogSeverity log_severity() const { return severity_; }

  int verbosity() const { return verbose_level_; }

  absl::Time timestamp() const { return timestamp_; }

  tid_t tid() const { return tid_; }

  absl::string_view text_message_with_prefix_and_newline() const
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return absl::string_view(
        text_message_with_prefix_and_newline_and_nul_.data(),
        text_message_with_prefix_and_newline_and_nul_.size() - 1);
  }
  absl::string_view text_message_with_prefix() const
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return absl::string_view(
        text_message_with_prefix_and_newline_and_nul_.data(),
        text_message_with_prefix_and_newline_and_nul_.size() - 2);
  }
  absl::string_view text_message_with_newline() const
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return absl::string_view(
        text_message_with_prefix_and_newline_and_nul_.data() + prefix_len_,
        text_message_with_prefix_and_newline_and_nul_.size() - prefix_len_ - 1);
  }
  absl::string_view text_message() const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return absl::string_view(
        text_message_with_prefix_and_newline_and_nul_.data() + prefix_len_,
        text_message_with_prefix_and_newline_and_nul_.size() - prefix_len_ - 2);
  }
  const char* text_message_with_prefix_and_newline_c_str() const
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return text_message_with_prefix_and_newline_and_nul_.data();
  }

  absl::string_view encoded_message() const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return encoding_;
  }

  absl::string_view stacktrace() const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return stacktrace_;
  }

 private:
  LogEntry() = default;

  absl::string_view full_filename_;
  absl::string_view base_filename_;
  int line_;
  bool prefix_;
  absl::LogSeverity severity_;
  int verbose_level_;  
  absl::Time timestamp_;
  tid_t tid_;
  absl::Span<const char> text_message_with_prefix_and_newline_and_nul_;
  size_t prefix_len_;
  absl::string_view encoding_;
  std::string stacktrace_;

  friend class log_internal::LogEntryTestPeer;
  friend class log_internal::LogMessage;
  friend void PrintTo(const absl::LogEntry& entry, std::ostream* os);
};

ABSL_NAMESPACE_END
}  

#endif  // ABSL_LOG_LOG_ENTRY_H_
