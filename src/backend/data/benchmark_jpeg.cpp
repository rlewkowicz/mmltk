
#include "detail/benchmark_jpeg.h"
#include <turbojpeg.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::benchmark_internal {
using mmltk::common::math::checked_cast;
namespace {
[[nodiscard]] std::size_t checked_pixel_bytes(const BenchmarkJpegHeader& header, const std::uint32_t channels) {
    const std::uint64_t pixels = static_cast<std::uint64_t>(header.width) * header.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / channels) { throw BenchmarkJpegError("benchmark JPEG decoded buffer size overflow"); }
    return static_cast<std::size_t>(pixels * channels);
}
[[nodiscard]] bool has_adobe_app14(const std::span<const std::uint8_t> encoded) noexcept {
    if (encoded.size() < 4U || encoded[0] != 0xFFU || encoded[1] != 0xD8U) { return false; }
    std::size_t offset = 2U;
    while (offset < encoded.size()) {
        while (offset < encoded.size() && encoded[offset] == 0xFFU) { ++offset; }
        if (offset >= encoded.size()) { return false; }
        const std::uint8_t marker = encoded[offset++];
        if (marker == 0xDAU || marker == 0xD9U) { return false; }
        if (marker == 0x00U || marker == 0x01U || (marker >= 0xD0U && marker <= 0xD7U)) { continue; }
        if (offset + 2U > encoded.size()) { return false; }
        const std::size_t segment_bytes = (static_cast<std::size_t>(encoded[offset]) << 8U) | encoded[offset + 1U];
        if (segment_bytes < 2U || segment_bytes > encoded.size() - offset) { return false; }
        const std::size_t payload = offset + 2U;
        if (marker == 0xEEU && segment_bytes >= 14U &&
            std::equal(encoded.begin() + static_cast<std::ptrdiff_t>(payload), encoded.begin() + static_cast<std::ptrdiff_t>(payload + 5U), "Adobe")) {
            return true;
        }
        offset += segment_bytes;
    }
    return false;
}
void convert_cmyk_to_rgb(const std::span<const std::uint8_t> cmyk, const bool inverted, const std::span<std::uint8_t> rgb) noexcept {
    const std::size_t pixels = rgb.size() / 3U;
    if (inverted) {
        for (std::size_t pixel = 0U; pixel < pixels; ++pixel) {
            const std::size_t source = pixel * 4U;
            const std::size_t destination = pixel * 3U;
            const std::uint32_t key = cmyk[source + 3U];
            rgb[destination] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(cmyk[source]) * key + 127U) / 255U);
            rgb[destination + 1U] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(cmyk[source + 1U]) * key + 127U) / 255U);
            rgb[destination + 2U] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(cmyk[source + 2U]) * key + 127U) / 255U);
        }
        return;
    }
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel) {
        const std::size_t source = pixel * 4U;
        const std::size_t destination = pixel * 3U;
        const std::uint32_t key = 255U - cmyk[source + 3U];
        rgb[destination] = static_cast<std::uint8_t>(((255U - cmyk[source]) * key + 127U) / 255U);
        rgb[destination + 1U] = static_cast<std::uint8_t>(((255U - cmyk[source + 1U]) * key + 127U) / 255U);
        rgb[destination + 2U] = static_cast<std::uint8_t>(((255U - cmyk[source + 2U]) * key + 127U) / 255U);
    }
}
}  // namespace
BenchmarkJpegDecoder::BenchmarkJpegDecoder() : handle_(tjInitDecompress()) {
    if (handle_ == nullptr) { throw BenchmarkJpegError("cannot initialize benchmark TurboJPEG decoder"); }
}
BenchmarkJpegDecoder::~BenchmarkJpegDecoder() {
    if (handle_ != nullptr) { (void)tjDestroy(handle_); }
}
BenchmarkJpegHeader BenchmarkJpegDecoder::read_header(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width,
                                                      const std::uint32_t expected_height) {
    if (encoded.empty()) { throw BenchmarkJpegError("benchmark JPEG is empty"); }
    int width = 0;
    int height = 0;
    int subsampling = 0;
    int colorspace = 0;
    if (encoded.size() > std::numeric_limits<unsigned long>::max()) { throw BenchmarkJpegError("benchmark JPEG size overflow"); }
    if (tjDecompressHeader3(handle_, encoded.data(), static_cast<unsigned long>(encoded.size()), &width, &height, &subsampling, &colorspace) < 0 ||
        width <= 0 || height <= 0) {
        throw BenchmarkJpegError(std::string("cannot read benchmark JPEG header: ") + tjGetErrorStr2(handle_));
    }
    const std::uint32_t actual_width = checked_cast<std::uint32_t>(width, "benchmark JPEG width overflow");
    const std::uint32_t actual_height = checked_cast<std::uint32_t>(height, "benchmark JPEG height overflow");
    if ((expected_width != 0U && actual_width != expected_width) || (expected_height != 0U && actual_height != expected_height)) {
        throw BenchmarkJpegError("benchmark JPEG dimensions do not match annotations");
    }
    return BenchmarkJpegHeader{actual_width, actual_height, colorspace, colorspace == TJCS_YCCK || (colorspace == TJCS_CMYK && has_adobe_app14(encoded))};
}
void BenchmarkJpegDecoder::decode_rgb(const std::span<const std::uint8_t> encoded, const BenchmarkJpegHeader& header, std::vector<std::uint8_t>* rgb,
                                      std::vector<std::uint8_t>* cmyk_scratch) {
    if (rgb == nullptr || cmyk_scratch == nullptr) { throw BenchmarkJpegError("benchmark JPEG decode buffers are missing"); }
    if (encoded.size() > std::numeric_limits<unsigned long>::max()) { throw BenchmarkJpegError("benchmark JPEG size overflow"); }
    rgb->resize(checked_pixel_bytes(header, 3U));
    const auto encoded_bytes = static_cast<unsigned long>(encoded.size());
    const int width = checked_cast<int>(header.width, "benchmark JPEG width overflow");
    const int height = checked_cast<int>(header.height, "benchmark JPEG height overflow");
    if (header.colorspace != TJCS_CMYK && header.colorspace != TJCS_YCCK) {
        if (tjDecompress2(handle_, encoded.data(), encoded_bytes, rgb->data(), width, 0, height, TJPF_RGB, 0) < 0) {
            throw BenchmarkJpegError(std::string("cannot decode benchmark JPEG: ") + tjGetErrorStr2(handle_));
        }
        return;
    }
    cmyk_scratch->resize(checked_pixel_bytes(header, 4U));
    if (tjDecompress2(handle_, encoded.data(), encoded_bytes, cmyk_scratch->data(), width, 0, height, TJPF_CMYK, 0) < 0) {
        throw BenchmarkJpegError(std::string("cannot decode benchmark CMYK JPEG: ") + tjGetErrorStr2(handle_));
    }
    convert_cmyk_to_rgb(*cmyk_scratch, header.inverted_cmyk, *rgb);
}
}  // namespace mmltk::backend::data::benchmark_internal
