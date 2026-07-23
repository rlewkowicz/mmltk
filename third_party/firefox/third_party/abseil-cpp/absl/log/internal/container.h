// Copyright 2025 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_LOG_INTERNAL_CONTAINER_H_
#define ABSL_LOG_INTERNAL_CONTAINER_H_

#include <cstdint>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>

#include "absl/base/config.h"
#include "absl/meta/internal/requires.h"
#include "absl/strings/str_cat.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace log_internal {


namespace internal {

struct LogBase {
  template <typename ElementT>
  void Log(std::ostream &out, const ElementT &element) const {  // NOLINT
    if constexpr (meta_internal::Requires<std::ostream, ElementT>(
                      [](auto&& x, auto&& y) -> decltype(x << y) {})) {
      out << element;
    } else {
      out << absl::StrCat(element);
    }
  }
  void LogEllipsis(std::ostream &out) const {  // NOLINT
    out << "...";
  }
};

struct LogShortBase : public LogBase {
  void LogOpening(std::ostream &out) const { out << "["; }        // NOLINT
  void LogClosing(std::ostream &out) const { out << "]"; }        // NOLINT
  void LogFirstSeparator(std::ostream &out) const { out << ""; }  // NOLINT
  void LogSeparator(std::ostream &out) const { out << ", "; }     // NOLINT
};

struct LogMultilineBase : public LogBase {
  void LogOpening(std::ostream &out) const { out << "["; }          // NOLINT
  void LogClosing(std::ostream &out) const { out << "\n]"; }        // NOLINT
  void LogFirstSeparator(std::ostream &out) const { out << "\n"; }  // NOLINT
  void LogSeparator(std::ostream &out) const { out << "\n"; }       // NOLINT
};

struct LogLegacyBase : public LogBase {
  void LogOpening(std::ostream &out) const { out << ""; }         // NOLINT
  void LogClosing(std::ostream &out) const { out << ""; }         // NOLINT
  void LogFirstSeparator(std::ostream &out) const { out << ""; }  // NOLINT
  void LogSeparator(std::ostream &out) const { out << " "; }      // NOLINT
};

}  

struct LogShort : public internal::LogShortBase {
  int64_t MaxElements() const { return (std::numeric_limits<int64_t>::max)(); }
};

class LogShortUpToN : public internal::LogShortBase {
 public:
  explicit LogShortUpToN(int64_t max_elements) : max_elements_(max_elements) {}
  int64_t MaxElements() const { return max_elements_; }

 private:
  int64_t max_elements_;
};

struct LogShortUpTo100 : public LogShortUpToN {
  LogShortUpTo100() : LogShortUpToN(100) {}
};

struct LogMultiline : public internal::LogMultilineBase {
  int64_t MaxElements() const { return (std::numeric_limits<int64_t>::max)(); }
};

class LogMultilineUpToN : public internal::LogMultilineBase {
 public:
  explicit LogMultilineUpToN(int64_t max_elements)
      : max_elements_(max_elements) {}
  int64_t MaxElements() const { return max_elements_; }

 private:
  int64_t max_elements_;
};

struct LogMultilineUpTo100 : public LogMultilineUpToN {
  LogMultilineUpTo100() : LogMultilineUpToN(100) {}
};

struct LogLegacyUpTo100 : public internal::LogLegacyBase {
  int64_t MaxElements() const { return 100; }
};
struct LogLegacy : public internal::LogLegacyBase {
  int64_t MaxElements() const { return (std::numeric_limits<int64_t>::max)(); }
};

typedef LogShortUpTo100 LogDefault;

template <typename IteratorT, typename PolicyT>
inline void LogRangeToStream(std::ostream &out,  // NOLINT
                             IteratorT begin, IteratorT end,
                             const PolicyT &policy) {
  policy.LogOpening(out);
  for (int64_t i = 0; begin != end && i < policy.MaxElements(); ++i, ++begin) {
    if (i == 0) {
      policy.LogFirstSeparator(out);
    } else {
      policy.LogSeparator(out);
    }
    policy.Log(out, *begin);
  }
  if (begin != end) {
    policy.LogSeparator(out);
    policy.LogEllipsis(out);
  }
  policy.LogClosing(out);
}

namespace detail {

template <typename IteratorT, typename PolicyT>
class RangeLogger {
 public:
  RangeLogger(const IteratorT &begin, const IteratorT &end,
              const PolicyT &policy)
      : begin_(begin), end_(end), policy_(policy) {}

  friend std::ostream &operator<<(std::ostream &out, const RangeLogger &range) {
    LogRangeToStream<IteratorT, PolicyT>(out, range.begin_, range.end_,
                                         range.policy_);
    return out;
  }

  std::string str() const {
    std::stringstream ss;
    ss << *this;
    return ss.str();
  }

 private:
  IteratorT begin_;
  IteratorT end_;
  PolicyT policy_;
};

template <typename E>
class EnumLogger {
 public:
  explicit EnumLogger(E e) : e_(e) {}

  friend std::ostream &operator<<(std::ostream &out, const EnumLogger &v) {
    using I = std::underlying_type_t<E>;
    return out << static_cast<I>(v.e_);
  }

 private:
  E e_;
};

}  

template <typename IteratorT, typename PolicyT>
detail::RangeLogger<IteratorT, PolicyT> LogRange(const IteratorT &begin,
                                                 const IteratorT &end,
                                                 const PolicyT &policy) {
  return detail::RangeLogger<IteratorT, PolicyT>(begin, end, policy);
}

template <typename IteratorT>
detail::RangeLogger<IteratorT, LogDefault> LogRange(const IteratorT &begin,
                                                    const IteratorT &end) {
  return LogRange(begin, end, LogDefault());
}

template <typename ContainerT, typename PolicyT>
auto LogContainer(const ContainerT& container, const PolicyT& policy)
    -> decltype(LogRange(container.begin(), container.end(), policy)) {
  return LogRange(container.begin(), container.end(), policy);
}

template <typename ContainerT>
auto LogContainer(const ContainerT& container)
    -> decltype(LogContainer(container, LogDefault())) {
  return LogContainer(container, LogDefault());
}

template <typename E>
detail::EnumLogger<E> LogEnum(E e) {
  static_assert(std::is_enum_v<E>, "must be an enum");
  return detail::EnumLogger<E>(e);
}

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_LOG_INTERNAL_CONTAINER_H_
