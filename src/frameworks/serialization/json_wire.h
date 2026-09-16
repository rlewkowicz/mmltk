#pragma once
#include <nlohmann/json.hpp>
#include <string_view>
#include "cbor_wire.h"
namespace mmltk::frameworks::serialization {
// JSON is an external representation of the canonical native wire vocabulary.
// These conversions contain no reflected record or field inventory.
[[nodiscard]] nlohmann::json json_from_cbor(wire::ByteSegments bytes, wire::Limits limits);
[[nodiscard]] wire::ByteBuffer json_to_cbor(std::string_view text, wire::Limits limits);
}  // namespace mmltk::frameworks::serialization
