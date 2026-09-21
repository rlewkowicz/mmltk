#include <sys/mman.h>
#include "src/common/io/file_memory.h"
#include "detail/benchmark_image_decoder.h"
#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <memory>
#include <stb_image.h>
#include <string>
#include <string_view>
#include <turbojpeg.h>
#include <span>
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::benchmark_internal {
using mmltk::common::math::checked_cast;
namespace {
[[nodiscard]] bool has_png_signature(const std::span<const std::uint8_t> encoded) noexcept {
 constexpr std::array<std::uint8_t, 8> signature{0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
 return encoded.size() >= signature.size() && std::ranges::equal(encoded.first(signature.size()), signature);
}
[[noreturn]] void png_error(const std::string_view action) {
 const char* reason = stbi_failure_reason();
 throw BenchmarkImageError(std::string(action) + ": " + (reason ? reason : "PNG decoding failed"));
}
[[nodiscard]] std::size_t checked_pixel_bytes(const BenchmarkImageHeader& header, const std::uint32_t channels) {
 const std::uint64_t pixels = static_cast<std::uint64_t>(header.width) * header.height;
 if (pixels > std::numeric_limits<std::size_t>::max() / channels) { throw BenchmarkImageError("benchmark image decoded buffer size overflow"); }
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
bool has_complete_image_markers(const std::span<const std::uint8_t> encoded) noexcept {
 if (has_png_signature(encoded)) {
  constexpr std::array<std::uint8_t, 12> end_chunk{0U, 0U, 0U, 0U, 'I', 'E', 'N', 'D', 0xAEU, 0x42U, 0x60U, 0x82U};
  return encoded.size() >= 57U && std::ranges::equal(encoded.last(end_chunk.size()), end_chunk);
 }
 if (encoded.size() < 4U || encoded[0] != 0xFFU || encoded[1] != 0xD8U) { return false; }
 std::size_t end = encoded.size();
 while (end > 2U && encoded[end - 1U] == 0xFFU) { --end; }
 return end >= 4U && encoded[end - 2U] == 0xFFU && encoded[end - 1U] == 0xD9U;
}
BenchmarkImageDecoder::BenchmarkImageDecoder() : handle_(tjInitDecompress()) {
 if (handle_ == nullptr) { throw BenchmarkImageError("cannot initialize benchmark TurboJPEG decoder"); }
}
BenchmarkImageDecoder::~BenchmarkImageDecoder() {
 if (handle_ != nullptr) { (void)tjDestroy(handle_); }
}
BenchmarkImageHeader BenchmarkImageDecoder::read_header(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width,
                                                        const std::uint32_t expected_height) {
 if (encoded.empty()) { throw BenchmarkImageError("benchmark image is empty"); }
 int width = 0;
 int height = 0;
 int subsampling = 0;
 int colorspace = 0;
 const auto encoding = has_png_signature(encoded) ? BenchmarkImageEncoding::Png : BenchmarkImageEncoding::Jpeg;
 if (encoding == BenchmarkImageEncoding::Png) {
  if (encoded.size() > INT_MAX) { throw BenchmarkImageError("benchmark PNG size overflow"); }
  int channels = 0;
  if (!stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels) || width <= 0 || height <= 0)
   png_error("cannot read benchmark PNG header");
 } else {
  if (encoded.size() > std::numeric_limits<unsigned long>::max()) { throw BenchmarkImageError("benchmark JPEG size overflow"); }
  if (tjDecompressHeader3(handle_, encoded.data(), static_cast<unsigned long>(encoded.size()), &width, &height, &subsampling, &colorspace) < 0 || width <= 0 ||
      height <= 0) {
   throw BenchmarkImageError(std::string("cannot read benchmark JPEG header: ") + tjGetErrorStr2(handle_));
  }
 }
 const std::uint32_t actual_width = checked_cast<std::uint32_t>(width, "benchmark image width overflow");
 const std::uint32_t actual_height = checked_cast<std::uint32_t>(height, "benchmark image height overflow");
 if ((expected_width != 0U && actual_width != expected_width) || (expected_height != 0U && actual_height != expected_height)) {
  throw BenchmarkImageError("benchmark image dimensions do not match annotations");
 }
 return BenchmarkImageHeader{actual_width, actual_height, colorspace,
                             encoding == BenchmarkImageEncoding::Jpeg && (colorspace == TJCS_YCCK || (colorspace == TJCS_CMYK && has_adobe_app14(encoded))),
                             encoding};
}
void BenchmarkImageDecoder::decode_rgb(const std::span<const std::uint8_t> encoded, const BenchmarkImageHeader& header, std::vector<std::uint8_t>* rgb,
                                       std::vector<std::uint8_t>* cmyk_scratch) {
 if (rgb == nullptr || cmyk_scratch == nullptr) { throw BenchmarkImageError("benchmark image decode buffers are missing"); }
 if (header.encoding == BenchmarkImageEncoding::Png) {
  if (encoded.size() > INT_MAX) { throw BenchmarkImageError("benchmark PNG size overflow"); }
  int width = 0, height = 0, channels = 0;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
   stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 3), stbi_image_free);
  if (!pixels) { png_error("cannot decode benchmark PNG"); }
  if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) != header.width || static_cast<std::uint32_t>(height) != header.height)
   throw BenchmarkImageError("decoded benchmark PNG dimensions do not match its header");
  rgb->assign(pixels.get(), pixels.get() + checked_pixel_bytes(header, 3U));
  return;
 }
 if (encoded.size() > std::numeric_limits<unsigned long>::max()) { throw BenchmarkImageError("benchmark JPEG size overflow"); }
 rgb->resize(checked_pixel_bytes(header, 3U));
 const auto encoded_bytes = static_cast<unsigned long>(encoded.size());
 const int width = checked_cast<int>(header.width, "benchmark JPEG width overflow");
 const int height = checked_cast<int>(header.height, "benchmark JPEG height overflow");
 if (header.colorspace != TJCS_CMYK && header.colorspace != TJCS_YCCK) {
  if (tjDecompress2(handle_, encoded.data(), encoded_bytes, rgb->data(), width, 0, height, TJPF_RGB, 0) < 0) {
   throw BenchmarkImageError(std::string("cannot decode benchmark JPEG: ") + tjGetErrorStr2(handle_));
  }
  return;
 }
 cmyk_scratch->resize(checked_pixel_bytes(header, 4U));
 if (tjDecompress2(handle_, encoded.data(), encoded_bytes, cmyk_scratch->data(), width, 0, height, TJPF_CMYK, 0) < 0) {
  throw BenchmarkImageError(std::string("cannot decode benchmark CMYK JPEG: ") + tjGetErrorStr2(handle_));
 }
 convert_cmyk_to_rgb(*cmyk_scratch, header.inverted_cmyk, *rgb);
}
namespace {
namespace common_io = mmltk::common::io;
class MappedImage {
public:
 explicit MappedImage(const std::filesystem::path& path) : file_(common_io::FileHandle::open_readonly(path.string())), size_(file_.size()) {
  if (size_ == 0U) { throw InvalidImageError("cached benchmark image is empty"); }
  data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, file_.get(), 0);
  if (data_ == MAP_FAILED) {
   data_ = nullptr;
   throw common_io::errno_error("cannot map cached benchmark image", path.string());
  }
 }
 ~MappedImage() {
  if (data_ != nullptr) { (void)::munmap(data_, size_); }
 }
 MappedImage(const MappedImage&) = delete;
 MappedImage& operator=(const MappedImage&) = delete;
 [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {static_cast<const std::uint8_t*>(data_), size_}; }

private:
 common_io::FileHandle file_;
 std::size_t size_ = 0U;
 void* data_ = nullptr;
};
[[nodiscard]] static MappedImage map_file(const std::filesystem::path& path) { return MappedImage(path); }
}  // namespace
std::pair<std::uint32_t, std::uint32_t> BenchmarkImageValidator::read_header(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width,
                                                                             const std::uint32_t expected_height) {
 try {
  const BenchmarkImageHeader header = decoder_.read_header(encoded, expected_width, expected_height);
  return {header.width, header.height};
 } catch (const BenchmarkImageError& error) { throw InvalidImageError(error.what()); }
}
std::pair<std::uint32_t, std::uint32_t> BenchmarkImageValidator::validate_file(const std::filesystem::path& path, const std::uint32_t expected_width,
                                                                               const std::uint32_t expected_height) {
 const MappedImage mapped = map_file(path);
 return read_header(mapped.bytes(), expected_width, expected_height);
}
void BenchmarkImageValidator::validate_decodable(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width,
                                                 const std::uint32_t expected_height) {
 try {
  const BenchmarkImageHeader header = decoder_.read_header(encoded, expected_width, expected_height);
  decoder_.decode_rgb(encoded, header, &decoded_, &cmyk_);
 } catch (const BenchmarkImageError& error) { throw InvalidImageError(std::string("cannot fully decode benchmark image: ") + error.what()); }
}
void BenchmarkImageValidator::validate_decodable_file(const std::filesystem::path& path, const std::uint32_t expected_width,
                                                      const std::uint32_t expected_height) {
 const MappedImage mapped = map_file(path);
 validate_decodable(mapped.bytes(), expected_width, expected_height);
}
}  // namespace mmltk::backend::data::benchmark_internal
