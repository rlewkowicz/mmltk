#pragma once  // backend.data private implementation boundary
#include <cstdint>
#include <filesystem>
#include <utility>
#include <span>
#include <stdexcept>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
enum class BenchmarkImageEncoding : std::uint8_t { Jpeg, Png };
struct BenchmarkImageHeader {
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
 int colorspace = 0;
 bool inverted_cmyk = false;
 BenchmarkImageEncoding encoding = BenchmarkImageEncoding::Jpeg;
};
// Encoded source content is authoritative; cache filenames retain their stable .jpg spelling.
[[nodiscard]] bool has_complete_image_markers(std::span<const std::uint8_t> encoded) noexcept;
class BenchmarkImageError : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
class BenchmarkImageDecoder {
public:
 BenchmarkImageDecoder();
 BenchmarkImageDecoder(const BenchmarkImageDecoder&) = delete;
 BenchmarkImageDecoder& operator=(const BenchmarkImageDecoder&) = delete;
 ~BenchmarkImageDecoder();
 [[nodiscard]] BenchmarkImageHeader read_header(std::span<const std::uint8_t> encoded, std::uint32_t expected_width = 0U, std::uint32_t expected_height = 0U);
 void decode_rgb(std::span<const std::uint8_t> encoded, const BenchmarkImageHeader& header, std::vector<std::uint8_t>* rgb, std::vector<std::uint8_t>* cmyk_scratch);

private:
 void* handle_ = nullptr;
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
 [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> read_header(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U);
 [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> validate_file(const std::filesystem::path& path, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U);
 void validate_decodable(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U);
 void validate_decodable_file(const std::filesystem::path& path, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U);

private:
 BenchmarkImageDecoder decoder_;
 std::vector<std::uint8_t> decoded_;
 std::vector<std::uint8_t> cmyk_;
};
}  // namespace mmltk::backend::data::benchmark_internal
