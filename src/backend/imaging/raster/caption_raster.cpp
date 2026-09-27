#include "caption_raster.h"
#include "src/common/types/utf8.h"
#include "detail/caption_raster_cuda.h"
#include "caption_font_data.h"
#include "src/frameworks/gpu/cuda/cuda_error.h"
#include "src/frameworks/gpu/memory/pinned_host_buffer.h"
#include <cstring>
#include <stb_truetype.h>
#include <cuda_runtime_api.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <iterator>
#include <vector>
namespace mmltk::backend::imaging::raster {
namespace gpu = mmltk::frameworks::gpu;
namespace {
constexpr std::string_view kSuffixGlyphs = " 0123456789.-+e";
std::vector<int> codepoints(const std::string& text) {
 std::vector<int> result;
 std::string_view remaining(text);
 while (!remaining.empty()) {
  const auto length = mmltk::common::types::utf8_prefix_length(remaining);
  if (!length) throw std::invalid_argument("invalid UTF-8 class caption");
  unsigned value = static_cast<unsigned char>(remaining.front());
  if (length > 1U) {
   value &= (1U << (7U - length)) - 1U;
   for (std::size_t index = 1; index < length; ++index) value = (value << 6U) | (static_cast<unsigned char>(remaining[index]) & 63U);
  }
  result.push_back(static_cast<int>(value));
  remaining.remove_prefix(length);
 }
 return result;
}
}  // namespace
struct CaptionRaster::Impl final {
 explicit Impl(gpu::DeviceContext owner) : context(owner), atlas(owner), commands(owner), winners(owner), upload(std::move(owner)) {
  if (!stbtt_InitFont(&face, kCaptionFont, 0)) throw std::runtime_error("bundled caption font is invalid");
 }
 void Allocate(gpu::ImageBuffer& buffer, gpu::BorrowedImageReadView& view, unsigned width, unsigned height) {
  if (view.valid() && view.plane().descriptor.width >= width && view.plane().descriptor.height >= height) return;
  view = {};
  buffer.Write(upload, gpu::ImagePlaneKind::Clean, width, height, [](auto, auto) {});
  upload.Synchronize();  // Allocation/growth only; ordinary draws share the caller stream.
  view = buffer.Borrow();
 }
 gpu::DeviceContext context;
 stbtt_fontinfo face{};
 std::vector<std::string> names;
 std::vector<unsigned> widths;
 std::array<unsigned, kSuffixGlyphs.size()> suffix_widths{};
 gpu::ImageBuffer atlas, commands, winners;
 std::unique_ptr<gpu::PinnedHostBuffer> atlas_source, command_source;
 gpu::ImageStream upload;
 gpu::BorrowedImageReadView retained, command_read, winner_read;
};
CaptionRaster::CaptionRaster(gpu::DeviceContext context) : impl_(std::make_unique<Impl>(std::move(context))) {}
CaptionRaster::~CaptionRaster() = default;
void CaptionRaster::Prepare(std::span<const std::string> names) {
 if (impl_->retained.valid() && std::ranges::equal(names, impl_->names)) return;
 if (names.empty() || names.size() > 65536U) throw std::invalid_argument("caption catalog is empty or too large");
 std::vector<std::vector<int>> points;
 std::vector<unsigned> widths;
 std::vector<std::string> catalog(names.begin(), names.end());
 for (const char glyph : kSuffixGlyphs) catalog.emplace_back(1U, glyph);
 const float scale = stbtt_ScaleForPixelHeight(&impl_->face, 16.0F);
 unsigned maximum = 1U;
 for (const auto& name : catalog) {
  if (name.size() > 256U) throw std::invalid_argument("caption name exceeds its bound");
  points.push_back(codepoints(name));
  float advance = 0;
  for (auto point : points.back()) {
   int width = 0, bearing = 0;
   stbtt_GetCodepointHMetrics(&impl_->face, point, &width, &bearing);
   advance += static_cast<float>(width) * scale;
  }
  widths.push_back(std::max(20U, static_cast<unsigned>(std::ceil(advance)) + 8U));
  maximum = std::max(maximum, widths.back());
 }
 // Store coverage in the first byte of each RGBA texel. ImageBuffer owns
 // context-aware allocation, upload settlement and high-water reuse.
 const auto coverage_bytes = static_cast<std::size_t>(maximum) * kNativeCaptionHeight * catalog.size() * 4U;
 if (coverage_bytes > 64U * 1024U * 1024U) throw std::invalid_argument("caption atlas exceeds its bounded storage");
 std::vector<std::uint8_t> pixels(coverage_bytes);
 std::vector<std::uint8_t> glyph;
 for (std::size_t row = 0; row < points.size(); ++row) {
  float pen = 4.0F;
  for (auto point : points[row]) {
   int advance = 0, bearing = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
   stbtt_GetCodepointHMetrics(&impl_->face, point, &advance, &bearing);
   stbtt_GetCodepointBitmapBox(&impl_->face, point, scale, scale, &x0, &y0, &x1, &y1);
   const int width = x1 - x0, height = y1 - y0;
   glyph.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
   if (width && height) stbtt_MakeCodepointBitmap(&impl_->face, glyph.data(), width, height, width, scale, scale, point);
   for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
     const int target_x = static_cast<int>(pen) + x0 + x, target_y = 17 + y0 + y;
     if (target_x >= 0 && target_x < static_cast<int>(widths[row]) && target_y >= 0 && target_y < static_cast<int>(kNativeCaptionHeight)) {
      auto& pixel = pixels[((row * kNativeCaptionHeight + static_cast<unsigned>(target_y)) * maximum + static_cast<unsigned>(target_x)) * 4U];
      pixel = std::max(pixel, glyph[static_cast<std::size_t>(y) * static_cast<unsigned>(width) + static_cast<unsigned>(x)]);
     }
    }
   pen += static_cast<float>(advance) * scale;
  }
 }
 impl_->context.Bind();
 if (!impl_->atlas_source) impl_->atlas_source = gpu::PinnedHostBuffer::ForCurrentDevice();
 impl_->atlas_source->ensure_bytes(pixels.size());
 std::memcpy(impl_->atlas_source->data(), pixels.data(), pixels.size());
 impl_->retained = {};
 try {
  impl_->atlas.Write(impl_->upload, gpu::ImagePlaneKind::Clean, maximum, static_cast<unsigned>(catalog.size()) * kNativeCaptionHeight, [&](auto plane, auto stream) {
   gpu::ensure_cuda_ok(cudaMemcpy2DAsync(reinterpret_cast<void*>(plane.data), plane.descriptor.pitch_bytes, impl_->atlas_source->data(), maximum * 4U, maximum * 4U, plane.descriptor.height,
                        cudaMemcpyHostToDevice, reinterpret_cast<cudaStream_t>(stream)),
    "upload bundled caption atlas");
  });
  // A catalog changes rarely. Settle this upload once before advertising it;
  // subsequent draws on any stream require no host wait or repeated upload.
  impl_->upload.Synchronize();
 } catch (...) { impl_->upload.RethrowAfterSettlement(std::current_exception()); }
 impl_->retained = impl_->atlas.Borrow();
 impl_->widths = std::move(widths);
 for (std::size_t index = 0; index < kSuffixGlyphs.size(); ++index) {
  int advance = 0, bearing = 0;
  stbtt_GetCodepointHMetrics(&impl_->face, kSuffixGlyphs[index], &advance, &bearing);
  impl_->suffix_widths[index] = std::max(1U, static_cast<unsigned>(std::ceil(static_cast<float>(advance) * scale)));
 }
 impl_->names.assign(names.begin(), names.end());
}
void CaptionRaster::Draw(gpu::ImagePlaneView target, std::span<const NamedCaption> captions, std::uintptr_t stream) const {
 if (!target.valid()) throw std::invalid_argument("caption output image is invalid");
 if (captions.empty()) return;
 if (!impl_->retained.valid()) throw std::logic_error("caption atlas has not been prepared");
 if (captions.size() > 8192U) throw std::invalid_argument("caption batch exceeds its object bound");
 impl_->context.Bind();
 const auto bytes = captions.size() * sizeof(detail::CaptionCommand);
 if (!impl_->command_source) impl_->command_source = gpu::PinnedHostBuffer::ForCurrentDevice();
 impl_->command_source->ensure_bytes(bytes);
 auto* commands = static_cast<detail::CaptionCommand*>(impl_->command_source->data());
 for (std::size_t index = 0; index < captions.size(); ++index) {
  const auto& item = captions[index];
  if (item.name >= impl_->names.size()) throw std::invalid_argument("caption category is absent");
  commands[index] = {item.name, impl_->widths[item.name], unsigned(item.background[0]) | (unsigned(item.background[1]) << 8U) | (unsigned(item.background[2]) << 16U), item.x, item.y};
  auto& command = commands[index];
  command.name_width = command.width;
  if (item.suffix.size() > std::size(command.suffix)) throw std::invalid_argument("caption suffix exceeds its bound");
  unsigned pen = command.width - 4U;
  for (const char glyph : item.suffix) {
   const auto ordinal = kSuffixGlyphs.find(glyph);
   if (ordinal == std::string_view::npos) throw std::invalid_argument("caption suffix must contain numeric ASCII glyphs");
   const auto row = static_cast<unsigned>(impl_->names.size() + ordinal);
   // Atlas rows have four-pixel margins. Advance is independent of the
   // minimum background width retained for ordinary class-only captions.
   const auto width = impl_->suffix_widths[ordinal];
   command.suffix[command.suffix_count++] = {row, pen, pen + width};
   pen += width;
  }
  if (command.suffix_count) command.width = pen + 4U;
 }
 impl_->Allocate(impl_->commands, impl_->command_read, static_cast<unsigned>((bytes + 3U) / 4U), 1U);
 impl_->Allocate(impl_->winners, impl_->winner_read, target.descriptor.width, target.descriptor.height);
 const auto command_plane = impl_->command_read.plane(), winner_plane = impl_->winner_read.plane(), atlas = impl_->retained.plane();
 // Descriptor upload and both ordered passes share the consuming image stream.
 // Caller settlement retains the pinned descriptors and all three allocations.
 gpu::ensure_cuda_ok(cudaMemcpyAsync(reinterpret_cast<void*>(command_plane.data), commands, bytes, cudaMemcpyHostToDevice, reinterpret_cast<cudaStream_t>(stream)), "upload caption batch");
 gpu::ensure_cuda_ok(static_cast<cudaError_t>(detail::paint_captions(reinterpret_cast<std::uint8_t*>(target.data), target.descriptor.pitch_bytes, target.descriptor.width, target.descriptor.height,
                      reinterpret_cast<const std::uint8_t*>(atlas.data), atlas.descriptor.pitch_bytes, reinterpret_cast<const detail::CaptionCommand*>(command_plane.data),
                      static_cast<unsigned>(captions.size()), reinterpret_cast<std::uint32_t*>(winner_plane.data), winner_plane.descriptor.pitch_bytes, reinterpret_cast<cudaStream_t>(stream))),
  "paint named output captions");
}
void CaptionRaster::Composite(gpu::ImagePlaneView clean, gpu::ImagePlaneView semantic, std::uintptr_t stream) {
 if (!clean.valid() || !semantic.valid() || clean.descriptor.width != semantic.descriptor.width || clean.descriptor.height != semantic.descriptor.height)
  throw std::invalid_argument("rendered output planes disagree");
 gpu::ensure_cuda_ok(static_cast<cudaError_t>(detail::composite_caption_source(reinterpret_cast<std::uint8_t*>(clean.data), clean.descriptor.pitch_bytes,
                      reinterpret_cast<const std::uint8_t*>(semantic.data), semantic.descriptor.pitch_bytes, clean.descriptor.width, clean.descriptor.height, reinterpret_cast<cudaStream_t>(stream))),
  "compose rendered output pixels");
}
}  // namespace mmltk::backend::imaging::raster
