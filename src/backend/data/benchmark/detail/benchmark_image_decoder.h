#pragma once  // backend.data private implementation boundary
#include "src/backend/data/detail/compile_image_decoder.h"
#include "src/common/system/numa_memory.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>
#include <span>
#include <stdexcept>
namespace mmltk::backend::data::benchmark_internal {
using BenchmarkImageEncoding = detail::CompileImageEncoding;
using BenchmarkImageHeader = detail::CompileImageHeader;
// Encoded source content is authoritative; cache filenames retain their stable .jpg spelling.
[[nodiscard]] bool has_complete_image_markers(std::span<const std::uint8_t> encoded) noexcept;
class BenchmarkImageError : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
class BenchmarkImageDecoder {
public:
 BenchmarkImageDecoder() noexcept = default;
 BenchmarkImageDecoder(const BenchmarkImageDecoder&) = delete;
 BenchmarkImageDecoder& operator=(const BenchmarkImageDecoder&) = delete;
 ~BenchmarkImageDecoder() = default;
 [[nodiscard]] BenchmarkImageHeader read_header(std::span<const std::uint8_t> encoded, std::uint32_t expected_width = 0U, std::uint32_t expected_height = 0U);
 [[nodiscard]] std::span<const std::uint8_t> decode_rgb(std::span<const std::uint8_t>, const BenchmarkImageHeader&, std::span<std::byte>);
 [[nodiscard]] static detail::CompileImagePlan plan(const BenchmarkImageHeader& header) { return detail::CompileImageDecoder::plan(header); }

private:
 detail::CompileImageDecoder decoder_;
};
class InvalidImageError : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
struct ImageDecodeProbe {
 std::uint64_t image_id = 0U;
 std::uint32_t expected_width = 0U;
 std::uint32_t expected_height = 0U;
};
class BenchmarkImageValidator {
public:
 [[nodiscard]] const BenchmarkImageHeader& admitted_header() const noexcept { return header_; }
 [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> read_header(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U);
 [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> validate_file(const std::filesystem::path& path, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U);
 void validate_decodable(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U);
 void validate_decodable_file(const std::filesystem::path& path, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U);

private:
 BenchmarkImageHeader header_;
 BenchmarkImageDecoder decoder_;
 std::unique_ptr<mmltk::common::system::NumaMemory> workspace_;
};
}  // namespace mmltk::backend::data::benchmark_internal
