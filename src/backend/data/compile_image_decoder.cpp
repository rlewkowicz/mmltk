#include "src/backend/data/detail/compile_image_decoder.h"
#include "src/backend/data/detail/compiled_label_workspace.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/common/math/checked_arithmetic.h"
#include <stb_image.h>
#include <turbojpeg.h>
#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <string>
#include <string_view>
namespace mmltk::backend::data::detail {
using mmltk::common::math::checked_cast;
[[nodiscard]] bool has_png_image_signature(const std::span<const std::uint8_t> encoded) noexcept {
 constexpr std::array<std::uint8_t, 8> signature{0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
 return encoded.size() >= signature.size() && std::ranges::equal(encoded.first(signature.size()), signature);
}
namespace {
[[noreturn]] void png_error(const std::string_view action) {
 const char* reason = stbi_failure_reason();
 throw CompileImageError(std::string(action) + ": " + (reason ? reason : "PNG decoding failed"));
}
[[nodiscard]] std::size_t checked_pixel_bytes(const CompileImageHeader& header, const std::uint32_t channels) {
 const std::uint64_t pixels = static_cast<std::uint64_t>(header.width) * header.height;
 if (pixels > std::numeric_limits<std::size_t>::max() / channels) { throw CompileImageError("benchmark image decoded buffer size overflow"); }
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
  if (marker == 0xEEU && segment_bytes >= 14U && std::equal(encoded.begin() + static_cast<std::ptrdiff_t>(payload), encoded.begin() + static_cast<std::ptrdiff_t>(payload + 5U), "Adobe")) {
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
void CompileImageDecoder::ensure_jpeg_handle() {
 if (handle_) return;
 handle_ = tjInitDecompress();
 if (!handle_) throw CompileImageError("cannot initialize benchmark TurboJPEG decoder");
}
CompileImageDecoder::~CompileImageDecoder() { if (handle_) (void)tjDestroy(handle_); }
CompileImageHeader CompileImageDecoder::read_header(const std::span<const std::uint8_t> encoded, const CompileImagePolicy policy, const std::uint32_t expected_width, const std::uint32_t expected_height) {
 if (encoded.empty()) { throw CompileImageError("benchmark image is empty"); }
 int width = 0;
 int height = 0;
 int subsampling = 0;
 int colorspace = 0;
 const auto encoding = policy == CompileImagePolicy::Directory ? CompileImageEncoding::Stb : has_png_image_signature(encoded) ? CompileImageEncoding::Png : CompileImageEncoding::Jpeg;
 if (encoding != CompileImageEncoding::Jpeg) {
  if (encoded.size() > INT_MAX) { throw CompileImageError("benchmark PNG size overflow"); }
  int channels = 0;
  if (!stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels) || width <= 0 || height <= 0) png_error("cannot read benchmark PNG header");
 } else {
  if (encoded.size() > std::numeric_limits<unsigned long>::max()) { throw CompileImageError("benchmark JPEG size overflow"); }
  ensure_jpeg_handle();
  if (tjDecompressHeader3(handle_, encoded.data(), static_cast<unsigned long>(encoded.size()), &width, &height, &subsampling, &colorspace) < 0 || width <= 0 || height <= 0) {
   throw CompileImageError(std::string("cannot read benchmark JPEG header: ") + tjGetErrorStr2(handle_));
  }
 }
 const std::uint32_t actual_width = checked_cast<std::uint32_t>(width, "benchmark image width overflow");
 const std::uint32_t actual_height = checked_cast<std::uint32_t>(height, "benchmark image height overflow");
 if ((expected_width != 0U && actual_width != expected_width) || (expected_height != 0U && actual_height != expected_height)) {
  throw CompileImageError("benchmark image dimensions do not match annotations");
 }
 // STB inspection and planning share bounded local codec controls. A valid
 // header whose decode plan cannot be represented is a resource failure.
 const auto decoder_bytes = encoding == CompileImageEncoding::Jpeg ? 0 : stbi_workspace_required(encoded.data(), static_cast<int>(encoded.size()), 3);
 if (encoding != CompileImageEncoding::Jpeg && !decoder_bytes) throw std::length_error("image decoder workspace size overflow");
 return CompileImageHeader{
  actual_width, actual_height, colorspace, encoding == CompileImageEncoding::Jpeg && (colorspace == TJCS_YCCK || (colorspace == TJCS_CMYK && has_adobe_app14(encoded))), encoding, decoder_bytes
 };
}
std::size_t CompileImagePlan::bytes() const {
 using mmltk::common::math::checked_add;
 return checked_add(checked_add(output_bytes,cmyk_bytes,"image workspace overflow"),stb_bytes,"image workspace overflow");
}
CompileImagePlan CompileImageDecoder::plan(const CompileImageHeader& header) {
 if (!header.width || !header.height) throw CompileImageError("image workspace has no admitted dimensions");
 if (header.encoding != CompileImageEncoding::Jpeg) {
  if (!header.decoder_bytes) throw CompileImageError("image decoder has no checked workspace plan");
  return {.stb_bytes = header.decoder_bytes};
 }
 // TurboJPEG owns opaque MCU/progressive coefficient bookkeeping. It cannot
 // allocate from a supplied arena. Charge four padded short-coefficient planes
 // and row/control storage separately from the RGB/CMYK payload we supply.
 using mmltk::common::math::checked_multiply;
 using mmltk::common::math::checked_add;
 const auto width = (std::size_t(header.width) + 31) / 32 * 32;
 const auto height = (std::size_t(header.height) + 31) / 32 * 32;
 const auto coefficients = checked_multiply(checked_multiply(width,height,"JPEG coefficients overflow"),4*sizeof(short),"JPEG coefficients overflow");
 return {checked_pixel_bytes(header,3), header.colorspace == TJCS_CMYK || header.colorspace == TJCS_YCCK ? checked_pixel_bytes(header,4) : 0,
  0, checked_add(coefficients, checked_add(checked_multiply(width,16,"JPEG rows overflow"),std::size_t{65536},"JPEG controls overflow"),"JPEG workspace overflow")};
}
std::span<const std::uint8_t> CompileImageDecoder::decode_rgb(std::span<const std::uint8_t> encoded, const CompileImageHeader& header, std::span<std::byte> workspace) {
 const auto required = plan(header);
 if (workspace.size() < required.bytes()) throw std::length_error("image decode exceeds supplied workspace");
 if (header.encoding != CompileImageEncoding::Jpeg) {
  if (encoded.size() > INT_MAX) throw CompileImageError("benchmark PNG size overflow");
  int width = 0, height = 0, channels = 0;
  auto* pixels = stbi_load_from_memory_workspace(encoded.data(),static_cast<int>(encoded.size()),&width,&height,&channels,3,workspace.data(),required.stb_bytes);
  if (!pixels) {
   const auto* reason = stbi_failure_reason();
   if (reason && std::string_view(reason) == "workspace capacity") throw std::length_error("supplied decoder workspace exhausted");
   png_error("cannot decode benchmark PNG");
  }
  if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) != header.width || static_cast<std::uint32_t>(height) != header.height)
   throw CompileImageError("decoded benchmark PNG dimensions do not match its header");
  return {pixels,checked_pixel_bytes(header,3)};
 }
 if (encoded.size() > std::numeric_limits<unsigned long>::max()) throw CompileImageError("benchmark JPEG size overflow");
 auto rgb = std::span(reinterpret_cast<std::uint8_t*>(workspace.data()),required.output_bytes);
 const auto bytes = static_cast<unsigned long>(encoded.size());
 const auto width = checked_cast<int>(header.width,"benchmark JPEG width overflow"), height = checked_cast<int>(header.height,"benchmark JPEG height overflow");
 ensure_jpeg_handle();
 if (!required.cmyk_bytes) {
  if (tjDecompress2(handle_,encoded.data(),bytes,rgb.data(),width,0,height,TJPF_RGB,0) < 0)
   throw CompileImageError(std::string("cannot decode benchmark JPEG: ")+tjGetErrorStr2(handle_));
 } else {
  auto cmyk = std::span(reinterpret_cast<std::uint8_t*>(workspace.data())+required.output_bytes,required.cmyk_bytes);
  if (tjDecompress2(handle_,encoded.data(),bytes,cmyk.data(),width,0,height,TJPF_CMYK,0) < 0)
   throw CompileImageError(std::string("cannot decode benchmark CMYK JPEG: ")+tjGetErrorStr2(handle_));
  convert_cmyk_to_rgb(cmyk,header.inverted_cmyk,rgb);
 }
 return rgb;
}
std::size_t CompilePixelWorkspacePlan::decoder_extent() const { return common::math::checked_add(decoder.bytes(),std::size_t{63},"decode alignment overflow")/64*64; }
std::size_t CompilePixelWorkspacePlan::bytes() const { return common::math::checked_add(decoder_extent(),resize.bytes(),"pixel workspace overflow"); }
CompileResources CompilePixelWorkspacePlan::resources() const {
 return {.bytes=bytes(),.supplied_workspace=true,.workspace_class=CompileWorkClass::Pixels,.opaque_bytes=decoder.opaque_bytes};
}
CompilePixelWorkspacePlan compile_pixel_workspace_plan(const CompileImageHeader& header,std::uint32_t resolution,imaging::resample::ImageResizeMode mode,bool perceptual) {
 return {CompileImageDecoder::plan(header),imaging::resample::RgbImageResizer::workspace_plan(header.width,header.height,resolution,resolution,mode,perceptual)};
}
CompileWorkspacePlan compile_workspace_plan(std::size_t workers,std::uint32_t resolution,bool perceptual,std::size_t reader_window,std::uint64_t explicit_budget,std::size_t encoded_window) {
 if (explicit_budget) return CompileWorkspacePlan::prepare(workers,{},0,0,explicit_budget);
 // Normal geometry chooses reusable capacity, never legal-input admission.
 // Actual larger shapes use their own checked decoder/resizer plan.
 constexpr std::uint32_t normal_extent=2048;
 const CompileImageHeader normal{normal_extent,normal_extent,0,false,CompileImageEncoding::Png,stbi_workspace_png_profile(normal_extent,normal_extent,encoded_window)};
 const auto pixels=compile_pixel_workspace_plan(normal,std::max(1U,resolution),imaging::resample::ImageResizeMode::Stretch,perceptual).bytes();
 const auto labels=CompiledLabelWorkspace::required_bytes(normal_extent);
 const std::array<CompileWorkspaceProfile,static_cast<std::size_t>(CompileWorkClass::Count)> profiles{{{64U<<10,64U<<10},{std::max(encoded_window,reader_window),64U<<10},
  {std::max(labels,reader_window),labels},{pixels,64U<<10},{labels,64U<<10},{labels,labels}}};
 return CompileWorkspacePlan::prepare(workers,profiles,encoded_window,COMPILED_COMPACTION_SCRATCH_BYTES);
}

}
