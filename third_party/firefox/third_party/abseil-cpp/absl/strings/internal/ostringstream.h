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

#ifndef ABSL_STRINGS_INTERNAL_OSTRINGSTREAM_H_
#define ABSL_STRINGS_INTERNAL_OSTRINGSTREAM_H_

#include <cassert>
#include <ios>
#include <ostream>
#include <streambuf>
#include <string>
#include <utility>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace strings_internal {

class OStringStream final : public std::ostream {
 public:
  explicit OStringStream(std::string* str)
      : std::ostream(&buf_), buf_(str) {}
  OStringStream(OStringStream&& that)
      : std::ostream(std::move(static_cast<std::ostream&>(that))),
        buf_(that.buf_) {
    rdbuf(&buf_);
  }
  OStringStream& operator=(OStringStream&& that) {
    std::ostream::operator=(std::move(static_cast<std::ostream&>(that)));
    buf_ = that.buf_;
    rdbuf(&buf_);
    return *this;
  }

  std::string* str() { return buf_.str(); }
  const std::string* str() const { return buf_.str(); }
  void str(std::string* str) { buf_.str(str); }

 private:
  class Streambuf final : public std::streambuf {
   public:
    explicit Streambuf(std::string* str) : str_(str) {}
    Streambuf(const Streambuf&) = default;
    Streambuf& operator=(const Streambuf&) = default;

    std::string* str() { return str_; }
    const std::string* str() const { return str_; }
    void str(std::string* str) { str_ = str; }

   protected:
    int_type overflow(int c) override;
    std::streamsize xsputn(const char* s, std::streamsize n) override;

   private:
    std::string* str_;
  } buf_;
};

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_INTERNAL_OSTRINGSTREAM_H_
