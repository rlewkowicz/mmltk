// Copyright 2024 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_LOG_INTERNAL_STRUCTURED_PROTO_H_
#define ABSL_LOG_INTERNAL_STRUCTURED_PROTO_H_

#include <cstddef>
#include <cstdint>
#include <variant>

#include "absl/base/config.h"
#include "absl/log/internal/proto.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace log_internal {

struct StructuredProtoField final {
  // Numeric type encoded with varint encoding:
  using Varint = std::variant<uint64_t, int64_t, uint32_t, int32_t, bool>;

  // Fixed-length 64-bit integer encoding:
  using I64 = std::variant<uint64_t, int64_t, double>;

  using LengthDelimited = absl::Span<const char>;

  // Fixed-length 32-bit integer encoding:
  using I32 = std::variant<uint32_t, int32_t, float>;

  using Value = std::variant<Varint, I64, LengthDelimited, I32>;

  uint64_t field_number;

  Value value;
};

inline size_t BufferSizeForStructuredProtoField(StructuredProtoField field) {
  struct BufferSizeVisitor final {
    size_t operator()(StructuredProtoField::Varint ) {
      return BufferSizeFor(field_number, WireType::kVarint);
    }

    size_t operator()(StructuredProtoField::I64 ) {
      return BufferSizeFor(field_number, WireType::k64Bit);
    }

    size_t operator()(StructuredProtoField::LengthDelimited length_delimited) {
      return BufferSizeFor(field_number, WireType::kLengthDelimited) +
             length_delimited.size();
    }

    size_t operator()(StructuredProtoField::I32 ) {
      return BufferSizeFor(field_number, WireType::k32Bit);
    }

    uint64_t field_number;
  };

  return std::visit(BufferSizeVisitor{field.field_number}, field.value);
}

bool EncodeStructuredProtoField(StructuredProtoField field,
                                absl::Span<char>& buf);

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_LOG_INTERNAL_STRUCTURED_PROTO_H_
