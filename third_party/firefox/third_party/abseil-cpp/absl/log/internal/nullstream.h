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

#if !defined(ABSL_LOG_INTERNAL_NULLSTREAM_H_)
#define ABSL_LOG_INTERNAL_NULLSTREAM_H_

#include <unistd.h>
#include <ios>
#include <ostream>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/log_severity.h"
#include "absl/strings/string_view.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace log_internal {

class NullStream {
 public:
  NullStream& AtLocation(absl::string_view, int) { return *this; }
  template <typename SourceLocationType>
  NullStream& AtLocation(SourceLocationType) {
    return *this;
  }
  NullStream& NoPrefix() { return *this; }
  NullStream& WithVerbosity(int) { return *this; }
  template <typename TimeType>
  NullStream& WithTimestamp(TimeType) {
    return *this;
  }
  template <typename Tid>
  NullStream& WithThreadID(Tid) {
    return *this;
  }
  template <typename LogEntryType>
  NullStream& WithMetadataFrom(const LogEntryType&) {
    return *this;
  }
  NullStream& WithPerror() { return *this; }
  template <typename LogSinkType>
  NullStream& ToSinkAlso(LogSinkType*) {
    return *this;
  }
  template <typename LogSinkType>
  NullStream& ToSinkOnly(LogSinkType*) {
    return *this;
  }
  template <typename LogSinkType>
  NullStream& OutputToSink(LogSinkType*, bool) {
    return *this;
  }
  NullStream& InternalStream() { return *this; }
  void Flush() {}
};
template <typename T>
inline NullStream& operator<<(NullStream& str, const T&) {
  return str;
}
inline NullStream& operator<<(NullStream& str,
                              std::ostream& (*)(std::ostream& os)) {
  return str;
}
inline NullStream& operator<<(NullStream& str,
                              std::ios_base& (*)(std::ios_base& os)) {
  return str;
}

class NullStreamMaybeFatal final : public NullStream {
 public:
  explicit NullStreamMaybeFatal(absl::LogSeverity severity)
      : fatal_(severity == absl::LogSeverity::kFatal) {}
  ~NullStreamMaybeFatal() {
    if (fatal_) {
      _exit(1);
    }
  }

 private:
  bool fatal_;
};

class NullStreamFatal final : public NullStream {
 public:
  NullStreamFatal() = default;
  [[noreturn]] ~NullStreamFatal() { _exit(1); }
};

}  
ABSL_NAMESPACE_END
}  

#endif
