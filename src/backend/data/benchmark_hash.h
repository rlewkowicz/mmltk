#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <optional>
#include <string>

#include "src/common/concurrency/cancellation_observation.h"

namespace mmltk::backend::data {

using Sha256Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] Sha256Digest sha256_bytes(std::span<const std::uint8_t> bytes);
[[nodiscard]] Sha256Digest sha256_file(const std::filesystem::path& path,
                                       mmltk::common::concurrency::CancellationObservation cancel_requested = {});
[[nodiscard]] std::optional<Sha256Digest> try_sha256_file(const std::filesystem::path& path,
                                                          mmltk::common::concurrency::CancellationObservation cancel_requested);
[[nodiscard]] std::string sha256_hex(const Sha256Digest& digest);
[[nodiscard]] Sha256Digest parse_sha256_hex(const std::string& value);
[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept;

}  // namespace mmltk::backend::data
