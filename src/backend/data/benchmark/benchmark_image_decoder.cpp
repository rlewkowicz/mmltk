#include "src/pch_linux.h"
#include "src/common/io/file_memory.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/pch_std.h"
#include "src/common/system/numa_topology.h"
namespace mmltk::backend::data::benchmark_internal {
bool has_complete_image_markers(const std::span<const std::uint8_t> encoded) noexcept {
 if (detail::has_png_image_signature(encoded)) {
  constexpr std::array<std::uint8_t, 12> end_chunk{0U, 0U, 0U, 0U, 'I', 'E', 'N', 'D', 0xAEU, 0x42U, 0x60U, 0x82U};
  return encoded.size() >= 57U && std::ranges::equal(encoded.last(end_chunk.size()), end_chunk);
 }
 if (encoded.size() < 4U || encoded[0] != 0xFFU || encoded[1] != 0xD8U) { return false; }
 std::size_t end = encoded.size();
 while (end > 2U && encoded[end - 1U] == 0xFFU) { --end; }
 return end >= 4U && encoded[end - 2U] == 0xFFU && encoded[end - 1U] == 0xD9U;
}
BenchmarkImageHeader BenchmarkImageDecoder::read_header(std::span<const std::uint8_t> encoded, std::uint32_t width, std::uint32_t height) {
 try {
  return decoder_.read_header(encoded, detail::CompileImagePolicy::Benchmark, width, height);
 } catch (const detail::CompileImageError& error) { throw BenchmarkImageError(error.what()); }
}
std::span<const std::uint8_t> BenchmarkImageDecoder::decode_rgb(std::span<const std::uint8_t> encoded, const BenchmarkImageHeader& header, std::span<std::byte> workspace) {
 try {
  return decoder_.decode_rgb(encoded, header, workspace);
 } catch (const detail::CompileImageError& error) { throw BenchmarkImageError(error.what()); }
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
std::pair<std::uint32_t, std::uint32_t> BenchmarkImageValidator::read_header(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width, const std::uint32_t expected_height) {
 try {
  header_ = decoder_.read_header(encoded, expected_width, expected_height);
  return {header_.width, header_.height};
 } catch (const BenchmarkImageError& error) { throw InvalidImageError(error.what()); }
}
std::pair<std::uint32_t, std::uint32_t> BenchmarkImageValidator::validate_file(const std::filesystem::path& path, const std::uint32_t expected_width, const std::uint32_t expected_height) {
 const MappedImage mapped = map_file(path);
 return read_header(mapped.bytes(), expected_width, expected_height);
}
void BenchmarkImageValidator::validate_decodable(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width, const std::uint32_t expected_height) {
 try {
  const BenchmarkImageHeader header = decoder_.read_header(encoded, expected_width, expected_height);
  if (!workspace_)
   workspace_ = std::make_unique<mmltk::common::system::NumaMemory>(mmltk::common::system::NumaTopology::Capture().permitted_nodes.front(), mmltk::common::system::NumaMemory::Lock::Pages);
  workspace_->ensure_bytes(BenchmarkImageDecoder::plan(header).bytes());
  (void)decoder_.decode_rgb(encoded, header, {static_cast<std::byte*>(workspace_->data()), workspace_->capacity_bytes()});
 } catch (const BenchmarkImageError& error) { throw InvalidImageError(std::string("cannot fully decode benchmark image: ") + error.what()); }
}
void BenchmarkImageValidator::validate_decodable_file(const std::filesystem::path& path, const std::uint32_t expected_width, const std::uint32_t expected_height) {
 const MappedImage mapped = map_file(path);
 validate_decodable(mapped.bytes(), expected_width, expected_height);
}
}  // namespace mmltk::backend::data::benchmark_internal
