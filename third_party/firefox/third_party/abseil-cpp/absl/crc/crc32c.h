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
#ifndef ABSL_CRC_CRC32C_H_
#define ABSL_CRC_CRC32C_H_

#include <cstdint>
#include <ostream>

#include "absl/crc/internal/crc32c_inline.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"

namespace absl {
ABSL_NAMESPACE_BEGIN


class crc32c_t final {
 public:
  crc32c_t() = default;
  constexpr explicit crc32c_t(uint32_t crc) : crc_(crc) {}

  crc32c_t(const crc32c_t&) = default;
  crc32c_t& operator=(const crc32c_t&) = default;

  explicit operator uint32_t() const { return crc_; }

  friend bool operator==(crc32c_t lhs, crc32c_t rhs) {
    return static_cast<uint32_t>(lhs) == static_cast<uint32_t>(rhs);
  }

  friend bool operator!=(crc32c_t lhs, crc32c_t rhs) { return !(lhs == rhs); }

  template <typename Sink>
  friend void AbslStringify(Sink& sink, crc32c_t crc) {
    absl::Format(&sink, "%08x", static_cast<uint32_t>(crc));
  }

 private:
  uint32_t crc_;
};


namespace crc_internal {
crc32c_t ExtendCrc32cInternal(crc32c_t initial_crc,
                              absl::string_view buf_to_add);
}  


inline crc32c_t ExtendCrc32c(crc32c_t initial_crc,
                             absl::string_view buf_to_add) {
  if (buf_to_add.size() <= 64) {
    uint32_t crc = static_cast<uint32_t>(initial_crc);
    if (crc_internal::ExtendCrc32cInline(&crc, buf_to_add.data(),
                                         buf_to_add.size())) {
      return crc32c_t{crc};
    }
  }
  return crc_internal::ExtendCrc32cInternal(initial_crc, buf_to_add);
}

inline crc32c_t ComputeCrc32c(absl::string_view buf) {
  return ExtendCrc32c(crc32c_t{0}, buf);
}

crc32c_t ExtendCrc32cByZeroes(crc32c_t initial_crc, size_t length);

crc32c_t MemcpyCrc32c(void* dest, const void* src, size_t count,
                      crc32c_t initial_crc = crc32c_t{0});



crc32c_t ConcatCrc32c(crc32c_t crc1, crc32c_t crc2, size_t crc2_length);

crc32c_t RemoveCrc32cPrefix(crc32c_t prefix_crc, crc32c_t full_string_crc,
                            size_t remaining_string_length);
crc32c_t RemoveCrc32cSuffix(crc32c_t full_string_crc, crc32c_t suffix_crc,
                            size_t suffix_length);

inline std::ostream& operator<<(std::ostream& os, crc32c_t crc) {
  return os << absl::StreamFormat("%08x", static_cast<uint32_t>(crc));
}

ABSL_NAMESPACE_END
}  

#endif  // ABSL_CRC_CRC32C_H_
