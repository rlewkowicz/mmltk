#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
namespace mmltk::backend::data::detail {
[[nodiscard]] bool has_png_image_signature(std::span<const std::uint8_t>) noexcept;
enum class CompileImageEncoding : std::uint8_t { Jpeg, Png, Stb };
enum class CompileImagePolicy : std::uint8_t { Benchmark, Directory };
struct CompileImageHeader {
 std::uint32_t width = 0, height = 0;
 int colorspace = 0;
 bool inverted_cmyk = false;
 CompileImageEncoding encoding = CompileImageEncoding::Jpeg;
 std::size_t decoder_bytes = 0;
 bool operator==(const CompileImageHeader&) const = default;
};
struct CompileImagePlan {
 std::size_t output_bytes = 0, cmyk_bytes = 0, stb_bytes = 0, opaque_bytes = 0;
 [[nodiscard]] std::size_t bytes() const;
};
class CompileImageError : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
class CompileImageDecoder final {
public:
 // Construction is resource-free; only benchmark JPEG operations own a handle.
 CompileImageDecoder() noexcept = default;
 ~CompileImageDecoder();
 CompileImageDecoder(const CompileImageDecoder&) = delete;
 CompileImageDecoder& operator=(const CompileImageDecoder&) = delete;
 [[nodiscard]] CompileImageHeader read_header(std::span<const std::uint8_t>, CompileImagePolicy, std::uint32_t expected_width = 0, std::uint32_t expected_height = 0);
 [[nodiscard]] static CompileImagePlan plan(const CompileImageHeader&);
 [[nodiscard]] std::span<const std::uint8_t> decode_rgb(std::span<const std::uint8_t>, const CompileImageHeader&, std::span<std::byte>);

private:
 void ensure_jpeg_handle();
 void* handle_ = nullptr;
};
}  // namespace mmltk::backend::data::detail
