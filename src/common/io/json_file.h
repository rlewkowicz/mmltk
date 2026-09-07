#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>

namespace mmltk::common::io {

enum class JsonWriteStage : std::uint8_t {
    kOpen = 0,
    kFlush = 1,
    kRename = 2,
};

// Appends one complete JSON value and its record delimiter. The caller owns the error policy so
// domain-specific writers can add context without reimplementing the stream operation.
[[nodiscard]] std::optional<JsonWriteStage> append_json_line(const std::filesystem::path& path, const nlohmann::json& payload);

// Writes a pre-encoded complete JSON value to `path` with a ".tmp" sibling
// followed by a rename. The caller may encode before accepting a transaction
// whose publication turn must be allocation-free.
[[nodiscard]] std::optional<JsonWriteStage> write_json_text_file_atomic(const std::filesystem::path& path, std::string_view encoded);

// Encodes and atomically writes one JSON value. Reports the failing stage
// instead of choosing an error policy.
[[nodiscard]] std::optional<JsonWriteStage> write_json_file_atomic(const std::filesystem::path& path, const nlohmann::json& payload,
                                                                   int indent = 2);

[[nodiscard]] const char* to_string(JsonWriteStage stage) noexcept;

void throw_on_json_write_failure(std::optional<JsonWriteStage> failure, const std::filesystem::path& path, std::string_view operation);

}  // namespace mmltk::common::io
