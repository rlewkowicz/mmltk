#include "src/backend/data/detail/compile_pixel_workspace.h"
#include "src/common/math/checked_arithmetic.h"
#include <stdexcept>
namespace mmltk::backend::data::detail {
CompilePixelWorkspacePlan::CompilePixelWorkspacePlan(const CompileImageHeader& header, CompilePixelPolicy policy)
    : header_(header),
      policy_(policy),
      decoder_(CompileImageDecoder::plan(header)),
      resize_(imaging::resample::RgbImageResizer::workspace_plan(header.width, header.height, policy.width, policy.height, policy.mode, policy.perceptual)) {
 using common::math::checked_add;
 using common::math::checked_multiply;
 decoder_extent_ = checked_add(decoder_.bytes(), std::size_t{63}, "decode alignment overflow") / 64 * 64;
 resources_ = {
  .bytes = checked_add(decoder_extent_, resize_.bytes(), "pixel workspace overflow"),
  .supplied_workspace = true,
  .completion_workspace = true,
  .workspace_class = CompileWorkClass::Pixels,
  .opaque_bytes = decoder_.opaque_bytes
 };
 const auto row = checked_multiply<std::size_t>(policy.width, sizeof(float), "pixel row overflow");
 const auto plane = checked_multiply<std::size_t>(row, policy.height, "pixel plane overflow");
 destination_ = {policy.width, policy.height, row, plane, checked_multiply<std::size_t>(plane, 3, "pixel destination overflow"), imaging::resample::RgbPixelFormat::PlanarUnitSrgbF32};
}
CompilePixelWorkspacePlan compile_pixel_workspace_plan(const CompileImageHeader& header, CompilePixelPolicy policy) { return CompilePixelWorkspacePlan(header, policy); }
CompilePixelWorkspacePlan compile_pixel_workspace_plan(const CompileImageHeader& header, std::uint32_t resolution, imaging::resample::ImageResizeMode mode, bool perceptual) {
 return compile_pixel_workspace_plan(header, {resolution, resolution, mode, perceptual});
}
void CompilePixelWorkspace::write(const CompilePixelWorkspacePlan& plan, std::span<const std::uint8_t> encoded, const CompileImageHeader& header, std::span<std::byte> memory,
 imaging::resample::RgbMutableImageView destination, std::function_ref<void()> prepare_destination) {
 if (header != plan.header_) throw CompileImageError("pixel input differs from its prepared header");
 const auto& expected = plan.destination_;
 const auto& actual = destination.layout;
 if (!destination.data || reinterpret_cast<std::uintptr_t>(destination.data) % alignof(float) || actual.width != expected.width || actual.height != expected.height ||
     actual.row_stride_bytes != expected.row_stride_bytes || actual.plane_stride_bytes != expected.plane_stride_bytes || actual.capacity_bytes < expected.capacity_bytes ||
     actual.format != expected.format)
  throw std::invalid_argument("pixel destination differs from its prepared layout");
 if (memory.size() < plan.bytes() || reinterpret_cast<std::uintptr_t>(memory.data()) % 64) throw std::length_error("pixel workspace capacity or alignment invalid");
 // JPEG decoding can otherwise scale an unexpected encoded geometry into a
 // supplied output extent. Verify actual content before any destination write.
 const auto policy = header.encoding == CompileImageEncoding::Stb ? CompileImagePolicy::Directory : CompileImagePolicy::Benchmark;
 if (decoder_.read_header(encoded, policy) != header) throw CompileImageError("pixel content differs from its prepared header");
 resizer_.prepare(plan.resize_, memory.subspan(plan.decoder_extent_, plan.resize_.bytes()));
 const auto decoded = decoder_.decode_rgb(encoded, header, memory.first(plan.decoder_.bytes()));
 prepare_destination();
 resizer_.resize_to_planar(
  {decoded.data(), {header.width, header.height, static_cast<std::size_t>(header.width) * 3U, 0U, decoded.size(), imaging::resample::RgbPixelFormat::RGB8}}, destination, plan.policy_.mode);
}
}  // namespace mmltk::backend::data::detail
