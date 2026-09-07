#pragma once  // backend.data private implementation boundary

#include <cstdint>
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

    [[nodiscard]] BenchmarkJpegHeader read_header(std::span<const std::uint8_t> encoded, std::uint32_t expected_width = 0U,
                                                  std::uint32_t expected_height = 0U);

    void decode_rgb(std::span<const std::uint8_t> encoded, const BenchmarkJpegHeader& header, std::vector<std::uint8_t>* rgb,
                    std::vector<std::uint8_t>* cmyk_scratch);

   private:
    void* handle_ = nullptr;
};

}  // namespace mmltk::backend::data::benchmark_internal
