#pragma once  // backend.data private implementation boundary
#include <cstdint>
#include <filesystem>
#include <utility>
#include <span>
#include <stdexcept>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
struct BenchmarkJpegHeader {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    int colorspace = 0;
    bool inverted_cmyk = false;
};
class BenchmarkJpegError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};
class BenchmarkJpegDecoder {
   public:
    BenchmarkJpegDecoder();
    BenchmarkJpegDecoder(const BenchmarkJpegDecoder&) = delete;
    BenchmarkJpegDecoder& operator=(const BenchmarkJpegDecoder&) = delete;
    ~BenchmarkJpegDecoder();
    [[nodiscard]] BenchmarkJpegHeader read_header(std::span<const std::uint8_t> encoded, std::uint32_t expected_width = 0U, std::uint32_t expected_height = 0U);
    void decode_rgb(std::span<const std::uint8_t> encoded, const BenchmarkJpegHeader& header, std::vector<std::uint8_t>* rgb,
                    std::vector<std::uint8_t>* cmyk_scratch);

   private:
    void* handle_ = nullptr;
};
class InvalidJpegError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};
struct JpegDecodeProbe {
    std::uint64_t image_id = 0U;
    std::uint32_t expected_width = 0U;
    std::uint32_t expected_height = 0U;
};
class JpegValidator {
   public:
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> read_header(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width = 0U,
                                                                      const std::uint32_t expected_height = 0U);
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> validate_file(const std::filesystem::path& path, const std::uint32_t expected_width = 0U,
                                                                        const std::uint32_t expected_height = 0U);
    void validate_decodable(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U);
    void validate_decodable_file(const std::filesystem::path& path, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U);

   private:
    BenchmarkJpegDecoder decoder_;
    std::vector<std::uint8_t> decoded_;
    std::vector<std::uint8_t> cmyk_;
};
}  // namespace mmltk::backend::data::benchmark_internal
