#pragma once
#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/data/detail/compile_workspace.h"
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
};
struct CompileImagePlan {
 std::size_t output_bytes = 0, cmyk_bytes = 0, stb_bytes = 0, opaque_bytes = 0;
 [[nodiscard]] std::size_t bytes() const;
};
struct CompilePixelWorkspacePlan {
 CompileImagePlan decoder;
 imaging::resample::RgbResizeWorkspacePlan resize;
 [[nodiscard]] std::size_t decoder_extent() const;
 [[nodiscard]] CompileResources resources() const;
 [[nodiscard]] std::size_t bytes() const;
};
[[nodiscard]] CompilePixelWorkspacePlan compile_pixel_workspace_plan(const CompileImageHeader&,std::uint32_t resolution,
 imaging::resample::ImageResizeMode,bool perceptual);
[[nodiscard]] CompileWorkspacePlan compile_workspace_plan(std::size_t workers,std::uint32_t resolution,bool perceptual,
 std::size_t reader_window = 0,std::uint64_t explicit_budget = 0,std::size_t encoded_window = 0);
class CompileImageError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
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
// Session-owned algorithm controls. All image-sized buffers bind to the current
// arena lease; a writer owns no decode or resampling workspace.
struct CompilePixelWorkspace final {
 CompileImageDecoder decoder;
 imaging::resample::RgbImageResizer resizer{1,false};
};

}
