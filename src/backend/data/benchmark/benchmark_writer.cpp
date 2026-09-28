#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/common/system/cpu_affinity.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_map>
#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/data/detail/writable_pixel_range.h"
#include "src/backend/data/compiled_file.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/concurrency/concurrency.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
// CLEANUP-IGNORE: This implementation imports the concrete data owners named by its independent compilation unit.
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
#include "src/backend/data/benchmark/detail/benchmark_images.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_writer.h"
namespace mmltk::backend::data::benchmark_internal {
namespace common_concurrency = mmltk::common::concurrency;
namespace common_io = mmltk::common::io;
namespace common_math = mmltk::common::math;
namespace {
[[nodiscard]] std::size_t checked_size_add(const std::size_t left, const std::size_t right, const char* context) { return common_math::checked_add(left, right, context); }
[[nodiscard]] std::size_t checked_size_multiply(const std::size_t left, const std::size_t right, const char* context) { return common_math::checked_multiply(left, right, context); }
using detail::WritablePixelRange;
class ReadOnlyMappedRange {
public:
 ReadOnlyMappedRange() = default;
 ReadOnlyMappedRange(const common_io::FileHandle& file, const std::size_t offset, const std::size_t bytes) {
  if (bytes == 0U) { return; }
  const std::size_t mapping_offset = offset & ~(PAGE_SIZE - 1U);
  const std::size_t delta = offset - mapping_offset;
  const std::size_t mapping_bytes = checked_size_add(delta, bytes, "benchmark validation mmap size overflow");
  void* mapping = ::mmap(nullptr, mapping_bytes, PROT_READ, MAP_SHARED, file.get(), common_math::checked_cast<off_t>(mapping_offset, "benchmark validation mmap offset overflow"));
  if (mapping == MAP_FAILED) { throw common_io::errno_error("benchmark validation mmap failed"); }
  region_.adopt(mapping, mapping_bytes);
  data_ = static_cast<const std::uint8_t*>(mapping) + delta;
 }
 ReadOnlyMappedRange(const ReadOnlyMappedRange&) = delete;
 ReadOnlyMappedRange& operator=(const ReadOnlyMappedRange&) = delete;
 [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }

private:
 common_io::MappedByteRegion region_;
 const std::uint8_t* data_ = nullptr;
};
[[nodiscard]] std::vector<ImageEntry> build_index(const PreparedBenchmarkSplit& split, const std::uint64_t pixel_offset, const std::uint64_t image_stride) {
 std::vector<ImageEntry> index(split.images.size());
 std::size_t expected_first_label = 0U;
 for (std::size_t image_index = 0U; image_index < split.images.size(); ++image_index) {
  const EncodedImageRecord& source = split.images[image_index];
  if (source.first_label != expected_first_label || expected_first_label > split.labels.size() || static_cast<std::size_t>(source.label_count) > split.labels.size() - expected_first_label) {
   throw std::runtime_error("benchmark split label ranges are not contiguous");
  }
  ImageEntry& destination = index[image_index];
  const std::size_t image_offset = checked_size_multiply(image_index, image_stride, "benchmark pixel index offset overflow");
  if (image_offset > std::numeric_limits<std::uint64_t>::max() - pixel_offset) { throw std::overflow_error("benchmark pixel index offset overflow"); }
  destination.pixel_offset = pixel_offset + image_offset;
  const std::size_t label_offset = checked_size_multiply(expected_first_label, sizeof(PackedInstance), "benchmark label offset overflow");
  destination.label_offset = common_math::checked_cast<std::uint32_t>(label_offset, "benchmark label offset overflow");
  destination.num_instances = source.label_count;
  destination.label_bytes = common_math::checked_cast<std::uint32_t>(static_cast<std::size_t>(source.label_count) * sizeof(PackedInstance), "benchmark label size overflow");
  destination.has_source_image_id = 1U;
  destination.source_image_id = source.source_image_id;
  destination.source = source.annotation_source;
  destination.original_width = source.source_width;
  destination.original_height = source.source_height;
  expected_first_label += source.label_count;
 }
 if (expected_first_label != split.labels.size()) { throw std::runtime_error("benchmark labels contain unreferenced records"); }
 return index;
}
}  // namespace
PackedInstance benchmark_canvas_box(
 const std::uint8_t class_id, const float x1, const float y1, const float x2, const float y2, const mmltk::backend::imaging::resample::ImageResizeGeometry& letterbox) {
 if (letterbox.resized_width == 0U || letterbox.resized_height == 0U) { throw std::runtime_error("benchmark box requires a valid letterbox"); }
 PackedInstance result{};
 result.class_id = class_id;
 result.bbox_x1 = x1 * static_cast<float>(letterbox.resized_width) + static_cast<float>(letterbox.offset_x);
 result.bbox_y1 = y1 * static_cast<float>(letterbox.resized_height) + static_cast<float>(letterbox.offset_y);
 result.bbox_x2 = x2 * static_cast<float>(letterbox.resized_width) + static_cast<float>(letterbox.offset_x);
 result.bbox_y2 = y2 * static_cast<float>(letterbox.resized_height) + static_cast<float>(letterbox.offset_y);
 return result;
}
struct BenchmarkPixelInput {
 BenchmarkSourcePublication publication;
 std::shared_ptr<const BenchmarkEncodedImage> payload;
 BenchmarkPixelInput(BenchmarkSourcePublication source, std::shared_ptr<const BenchmarkEncodedImage> image)
  : publication(std::move(source)), payload(std::move(image)) {}
};
struct BenchmarkSplitWriter::Impl {
 struct Scratch {
  BenchmarkImageDecoder decoder;
  mmltk::backend::imaging::resample::RgbImageResizer resizer;
  std::vector<std::uint8_t> decoded, cmyk;
  explicit Scratch(bool perceptual) : resizer(1, perceptual) {}
 };
 std::vector<EncodedImageRecord> images;
 std::vector<CachedImageSource> sources;
 std::vector<std::uint8_t> complete, header_known;
 mutable std::mutex facts_mutex;
 std::vector<std::unique_ptr<Scratch>> scratch;
 Scratch& lane_scratch(std::size_t lane) {
  auto& value = scratch.at(lane);
  if (!value) value = std::make_unique<Scratch>(perceptual);
  return *value;
 }
 BenchmarkCompilePipeline* execution = nullptr;
 BenchmarkAllowance writer_handles;
 std::mutex directory_mutex;
 common_io::FileHandle directory;
 std::uint16_t directory_source = std::numeric_limits<std::uint16_t>::max();
 BenchmarkStagedArtifact staging;
 std::unique_ptr<WritablePixelRange> pixels;
 FileLayout layout;
 std::uint32_t resolution;
 mmltk::backend::imaging::resample::ImageResizeMode resize_mode;
 common_concurrency::CancellationObservation cancellation;
 BenchmarkWriteProgressEvent progress;
 BenchmarkImageReadObserver image_opened;
 bool actual_dimensions;
 bool perceptual;
 std::size_t stride;
 Impl(const BenchmarkWriteRequest& request, bool actual)
     : images(request.split.images),
       sources(request.split.sources),
       complete(images.size()),
       header_known(images.size()),
       resolution(request.resolution),
       resize_mode(request.resize_mode),
       cancellation(request.cancel_requested),
       progress(request.progress),
       image_opened(request.image_opened),
       actual_dimensions(actual),
       perceptual(request.perceptual_downscale),
       stride(common_math::checked_cast<std::size_t>(static_cast<std::uint64_t>(resolution) * resolution * 3U * sizeof(float), "benchmark image stride overflow")) {
  execution = request.execution;
  if (execution) writer_handles = execution->reserve(BenchmarkResources::handles(2));
  if (resolution == 0 || resolution > MAX_IMAGE_EXTENT || images.empty() || sources.empty()) throw std::runtime_error("benchmark pixel membership is incomplete");
  layout = compute_pixel_layout(common_math::checked_cast<std::uint32_t>(images.size(), "benchmark image count overflow"), stride);
  (void)common_io::ensure_parent_directory(request.output_path);
  StorageReservationPool storage(request.output_path, {}, execution ? &execution->storage() : nullptr);
  staging = BenchmarkStagedArtifact::create(storage, request.output_path, layout.pixel_offset + layout.pixel_blob_size, "benchmark pixel staging");
  staging.preallocate(layout.pixel_offset + layout.pixel_blob_size);
  pixels = std::make_unique<WritablePixelRange>(staging.file().get(), layout.pixel_offset, layout.pixel_blob_size);
  const auto lanes = std::max(1, request.num_workers);
  scratch.reserve(lanes);
  for (int i = 0; i < lanes; ++i) scratch.push_back(std::make_unique<Scratch>(request.perceptual_downscale));
 }
 [[nodiscard]] std::vector<std::size_t> slots(const PreparedBenchmarkSplit& split) const {
  std::vector<std::size_t> result;
  result.reserve(split.images.size());
  std::size_t next = 0;
  for (const auto& image : split.images) {
   if (image.source_index >= split.sources.size()) throw std::runtime_error("benchmark final source index is invalid");
   for (; next < images.size(); ++next) {
    const auto& candidate = images[next];
    if (candidate.source_index >= sources.size()) throw std::runtime_error("benchmark initial source index is invalid");
    if (candidate.source_image_id == image.source_image_id && sources[candidate.source_index].root == split.sources[image.source_index].root) break;
   }
   if (next == images.size()) throw std::runtime_error("benchmark final membership changed canonical slot order");
   result.push_back(next++);
  }
  return result;
 }
};
BenchmarkSplitWriter::BenchmarkSplitWriter(const BenchmarkWriteRequest& request, bool actual) : impl_(std::make_unique<Impl>(request, actual)) {}
BenchmarkSplitWriter::~BenchmarkSplitWriter() = default;
void BenchmarkSplitWriter::retire_scratch(std::size_t lane) noexcept { impl_->scratch[lane].reset(); }
void BenchmarkSplitWriter::prepare_lanes(std::size_t lanes) {
 while (impl_->scratch.size() < lanes) impl_->scratch.push_back(std::make_unique<Impl::Scratch>(impl_->perceptual));
}
std::shared_ptr<BenchmarkPixelInput> BenchmarkSplitWriter::prepare_pixel(std::size_t slot, std::size_t lane,
 BenchmarkSourcePublication publication, BenchmarkAllowance allowance, std::shared_ptr<const BenchmarkEncodedImage> payload) {
 auto& state = *impl_;
 auto& image = state.images.at(slot);
 if (image_complete(slot)) return {};
 throw_if_benchmark_cancelled(state.cancellation);
 auto& scratch = state.lane_scratch(lane);
 auto input = std::make_shared<BenchmarkPixelInput>(std::move(publication), std::move(payload));
 try {
  if (!input->payload || input->payload->encoded().empty()) {
   common_io::FileHandle file;
   {
    const std::lock_guard lock(state.directory_mutex);
    if (state.directory_source != image.source_index) {
     state.directory = {};
     state.directory_source = std::numeric_limits<std::uint16_t>::max();
     const auto& root = state.sources.at(image.source_index).root;
     const int descriptor = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
     if (descriptor < 0) throw common_io::errno_error("cannot open cached benchmark image directory", root.string());
     state.directory = common_io::FileHandle(descriptor);
     state.directory_source = image.source_index;
    }
    std::array<char, 24> relative{}; (void)format_cached_image_relative_path(image.source_image_id, relative);
    const int descriptor = ::openat(state.directory.get(), relative.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) throw common_io::errno_error("cannot open cached benchmark image", relative.data());
    file = common_io::FileHandle(descriptor);
   }
   const CachedImageValidator header = [&](std::uint64_t, std::span<const std::uint8_t> encoded) {
    return scratch.decoder.read_header(encoded, state.actual_dimensions ? 0 : image.source_width, state.actual_dimensions ? 0 : image.source_height);
   };
   input->payload = BenchmarkEncodedImage::open(std::move(file), image.source_image_id, header, state.cancellation, nullptr,
    std::move(allowance), input->payload, std::numeric_limits<std::uint32_t>::max());
   if (!input->payload) throw std::runtime_error("cached benchmark image is missing or has an invalid size");
  }
  const auto& header = input->payload->header();
  if (!state.actual_dimensions && ((image.source_width && image.source_width != header.width) || (image.source_height && image.source_height != header.height)))
   throw BenchmarkImageError("benchmark image dimensions do not match annotations");
  if (state.image_opened) state.image_opened(state.sources.at(image.source_index).root, image.source_image_id);
  {
   const std::lock_guard lock(state.facts_mutex);
   image.source_width = header.width;
   image.source_height = header.height;
   state.header_known[slot] = 1;
  }
 } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) {
  throw_if_benchmark_cancelled(state.cancellation);
  throw BenchmarkImageReadError(image.source_index, image.source_image_id, error.what());
 }
 return input;
}
BenchmarkAllowance BenchmarkSplitWriter::pixel_input_allowance(const BenchmarkPixelInput& input) const { return input.payload->allowance(); }
std::uint64_t BenchmarkSplitWriter::pixel_workspace_bytes(const BenchmarkPixelInput& input) const { return pixel_workspace_bytes(input.payload->header(), input.payload->charged_bytes() ? 0 : input.payload->size()); }
std::uint64_t BenchmarkSplitWriter::pixel_workspace_bytes(const BenchmarkImageHeader& header, std::size_t encoded_bytes) const {
 // Source RGB/CMYK, decoder workspace and conservative perceptual filtering
 // intermediates, plus the mapped source working set. Final mmap pixels are a
 // retained product, not transient memory. Arithmetic never limits legal size.
 const auto source_pixels = common_math::checked_multiply(std::uint64_t{header.width}, header.height, "benchmark decode workspace overflow");
 return common_math::checked_add(encoded_bytes,
  common_math::checked_add(common_math::checked_multiply(source_pixels, 96U, "benchmark decode workspace overflow"),
   common_math::checked_multiply(std::uint64_t{impl_->resolution} * impl_->resolution, 96U, "benchmark resize workspace overflow"), "benchmark image workspace overflow"),
  "benchmark image workspace overflow");
}
void BenchmarkSplitWriter::write_pixel(std::size_t slot, std::size_t lane) {
 auto input = prepare_pixel(slot, lane);
 if (input) write_pixel(slot, lane, input);
}
void BenchmarkSplitWriter::write_pixel(std::size_t slot, std::size_t lane, const std::shared_ptr<BenchmarkPixelInput>& input) {
 if (!input || image_complete(slot)) return;
 auto& state = *impl_;
 auto& scratch = state.lane_scratch(lane);
 const auto& header = input->payload->header();
 throw_if_benchmark_cancelled(state.cancellation);
 std::span<const std::uint8_t> decoded;
 try {
  decoded = scratch.decoder.decode_rgb(input->payload->encoded(), header, &scratch.decoded, &scratch.cmyk);
 } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) {
  throw_if_benchmark_cancelled(state.cancellation);
  const auto& image = state.images.at(slot);
  throw BenchmarkImageReadError(image.source_index, image.source_image_id, error.what());
 }
 scratch.resizer.resize_to_planar(
  {decoded.data(), {header.width, header.height, static_cast<std::size_t>(header.width) * 3U, 0U, decoded.size(), mmltk::backend::imaging::resample::RgbPixelFormat::RGB8}},
  {state.pixels->image(common_math::checked_cast<std::uint32_t>(slot, "benchmark pixel slot overflow"), state.stride),
   {state.resolution, state.resolution, static_cast<std::size_t>(state.resolution) * sizeof(float), static_cast<std::size_t>(state.resolution) * state.resolution * sizeof(float), state.stride,
    mmltk::backend::imaging::resample::RgbPixelFormat::PlanarUnitSrgbF32}}, state.resize_mode);
 state.staging.reconcile();
 BenchmarkWriteProgressEvent progress;
 {
  const std::lock_guard lock(state.facts_mutex);
  state.complete[slot] = 1;
  progress = state.progress;
 }
 if (progress) progress();
}
std::uint64_t BenchmarkSplitWriter::allocated_bytes() const {
 struct stat status{};
 if (::fstat(impl_->staging.file().get(), &status) != 0) throw common_io::errno_error("cannot inspect benchmark staging allocation");
 return common_math::checked_multiply(common_math::checked_cast<std::uint64_t>(status.st_blocks, "benchmark allocation overflow"), std::uint64_t{512}, "benchmark allocation overflow");
}
std::optional<std::pair<std::uint32_t, std::uint32_t>> BenchmarkSplitWriter::header_dimensions(std::size_t slot) const {
 const std::lock_guard lock(impl_->facts_mutex);
 if (!impl_->header_known.at(slot)) return std::nullopt;
 const auto& image = impl_->images.at(slot);
 return std::pair{image.source_width, image.source_height};
}
bool BenchmarkSplitWriter::image_complete(std::size_t slot) const { const std::lock_guard lock(impl_->facts_mutex); return impl_->complete.at(slot) != 0; }
std::pair<std::uint32_t, std::uint32_t> BenchmarkSplitWriter::dimensions(std::size_t slot) const {
 const std::lock_guard lock(impl_->facts_mutex);
 if (!impl_->complete.at(slot)) throw std::logic_error("benchmark image dimensions requested before pixel completion");
 const auto& image = impl_->images.at(slot);
 return {image.source_width, image.source_height};
}
std::size_t BenchmarkSplitWriter::completed() const noexcept { const std::lock_guard lock(impl_->facts_mutex); return std::ranges::count(impl_->complete, std::uint8_t{1}); }
bool BenchmarkSplitWriter::matches_membership(const PreparedBenchmarkSplit& split) const {
 if (split.images.size() != impl_->images.size()) return false;
 for (std::size_t i = 0; i < split.images.size(); ++i) {
  const auto& before = impl_->images[i];
  const auto& after = split.images[i];
  if (before.source_image_id != after.source_image_id || impl_->sources.at(before.source_index).root != split.sources.at(after.source_index).root) return false;
 }
 return true;
}
void BenchmarkSplitWriter::invalidate_image(std::size_t slot) {
 bool invalidated;
 BenchmarkWriteProgressEvent progress;
 {
  const std::lock_guard lock(impl_->facts_mutex);
  invalidated = impl_->complete.at(slot) != 0;
  impl_->complete[slot] = impl_->header_known[slot] = 0;
  progress = impl_->progress;
 }
 if (invalidated && progress.images_invalidated) progress.images_invalidated(progress.context, 1);
}
void BenchmarkSplitWriter::invalidate_source(const std::filesystem::path& root) {
 {
  const std::lock_guard lock(impl_->directory_mutex);
  if (impl_->directory.get() >= 0 && impl_->sources.at(impl_->directory_source).root == root) {
   impl_->directory = {};
   impl_->directory_source = std::numeric_limits<std::uint16_t>::max();
  }
 }
 std::uint64_t invalidated = 0;
 BenchmarkWriteProgressEvent progress;
 {
 const std::lock_guard lock(impl_->facts_mutex);
 progress = impl_->progress;
 for (std::size_t i = 0; i < impl_->images.size(); ++i) {
  if (impl_->sources.at(impl_->images[i].source_index).root == root) {
   if (progress.images_invalidated) invalidated += impl_->complete[i] != 0;
   impl_->complete[i] = 0;
   impl_->header_known[i] = 0;
  }
 }
 }
 if (invalidated && progress.images_invalidated) progress.images_invalidated(progress.context, invalidated);
}
void BenchmarkSplitWriter::retain_completed(const BenchmarkSplitWriter& previous) {
 const auto& before = *previous.impl_;
 auto& after = *impl_;
 if (before.resolution != after.resolution || before.resize_mode != after.resize_mode) throw std::logic_error("benchmark pixel reuse changed geometry");
 std::unordered_map<std::string, std::unordered_map<std::uint64_t, std::size_t>> retained;
 for (std::size_t slot = 0; slot < before.images.size(); ++slot) {
  if (!before.complete[slot]) continue;
  const auto& image = before.images[slot];
  retained[before.sources.at(image.source_index).root.string()].emplace(image.source_image_id, slot);
 }
 std::vector<std::uint8_t> buffer(std::min<std::size_t>(after.stride, 1024U * 1024U));
 for (std::size_t slot = 0; slot < after.images.size(); ++slot) {
  throw_if_benchmark_cancelled(after.cancellation);
  auto& image = after.images[slot];
  const auto source = retained.find(after.sources.at(image.source_index).root.string());
  if (source == retained.end()) continue;
  const auto found = source->second.find(image.source_image_id);
  if (found == source->second.end()) continue;
  const auto& old_image = before.images[found->second];
  if (!after.actual_dimensions && ((image.source_width && image.source_width != old_image.source_width) || (image.source_height && image.source_height != old_image.source_height))) continue;
  const auto input = before.layout.pixel_offset + found->second * before.stride;
  const auto output = after.layout.pixel_offset + slot * after.stride;
  for (std::size_t offset = 0; offset < after.stride;) {
   const auto bytes = std::min(buffer.size(), after.stride - offset);
   before.staging.file().pread_all(buffer.data(), bytes, input + offset);
   after.staging.file().pwrite_all(buffer.data(), bytes, output + offset);
   offset += bytes;
  }
  after.staging.reconcile();
  image.source_width = old_image.source_width;
  image.source_height = old_image.source_height;
  after.complete[slot] = 1;
  after.header_known[slot] = 1;
 }
}
void BenchmarkSplitWriter::write_remaining(const BenchmarkWriteRequest& request) {
 { const std::lock_guard lock(impl_->facts_mutex); impl_->progress = request.progress; }
 auto slots = impl_->slots(request.split);
 std::erase_if(slots, [&](std::size_t slot) { return image_complete(slot); });
 if (slots.empty()) return;
 if (impl_->execution) {
  impl_->execution->write_remaining(*this, request.split, slots);
  return;
 }
 std::atomic<std::size_t> next{0};
 std::atomic<bool> failed{false};
 const auto cpus = request.worker_cpus.empty() ? mmltk::common::system::allowed_cpu_set() : std::vector<int>(request.worker_cpus.begin(), request.worker_cpus.end());
 const int workers = std::max(
  1, std::min({request.num_workers, common_math::checked_cast<int>(cpus.size(), "benchmark CPU count overflow"), common_math::checked_cast<int>(slots.size(), "benchmark slot count overflow")}));
 while (impl_->scratch.size() < static_cast<std::size_t>(workers)) impl_->scratch.push_back(std::make_unique<Impl::Scratch>(impl_->perceptual));
 const auto run = [&](int lane, int, int) {
  try {
   while (!failed.load(std::memory_order_relaxed)) {
    const auto index = next.fetch_add(1, std::memory_order_relaxed);
    if (index >= slots.size()) break;
    write_pixel(slots[index], static_cast<std::size_t>(lane));
   }
  } catch (...) {
   failed.store(true, std::memory_order_relaxed);
   throw;
  }
 };
 if (request.worker_cpus.empty())
  common_concurrency::parallel_for_range_indexed<int>(0, workers, workers, run);
 else
  common_concurrency::parallel_for_range_indexed<int>(0, workers, workers, request.worker_cpus, run);
}
void BenchmarkSplitWriter::finish(const BenchmarkWriteRequest& request) {
 auto& state = *impl_;
 if (request.resolution != state.resolution || request.resize_mode != state.resize_mode) throw std::runtime_error("benchmark final pixel geometry changed");
 const auto slots = state.slots(request.split);
 for (std::size_t i = 0; i < slots.size(); ++i) {
  if (!state.complete[slots[i]]) throw std::runtime_error("benchmark publication has unfinished pixels");
  const auto& final = request.split.images[i];
  if (dimensions(slots[i]) != std::pair{final.source_width, final.source_height}) throw std::runtime_error("benchmark labels and pixels disagree on source dimensions");
 }
 mmltk::common::logging::ScopedProfile profile{"benchmark.writer.total"};
 if (request.resolution == 0U || request.resolution > MAX_IMAGE_EXTENT) { throw std::runtime_error("benchmark resolution exceeds the compiled coordinate format"); }
 if (request.split.images.empty() || request.split.class_names.empty()) { throw std::runtime_error("benchmark split must contain images and classes"); }
 const catalog::ClassCatalog class_catalog(request.split.class_names, COMPILED_CLASS_NAME_CAPACITY);
 if (request.split.class_names.size() > MAX_CLASSES) { throw std::runtime_error("benchmark split exceeds the compiled class limit"); }
 if (request.split.sources.empty()) { throw std::runtime_error("benchmark split has no cached image sources"); }
 const std::uint64_t image_stride_u64 = static_cast<std::uint64_t>(request.resolution) * request.resolution * 3U * sizeof(float);
 const std::size_t image_stride = common_math::checked_cast<std::size_t>(image_stride_u64, "benchmark image stride overflow");
 const auto image_count = common_math::checked_cast<std::uint32_t>(request.split.images.size(), "benchmark image count overflow");
 auto layout = compute_pixel_layout(image_count, image_stride);
 finalize_layout(layout, {request.split.labels.size(), request.split.rle_pairs.size()});
 std::vector<ImageEntry> index = build_index(request.split, layout.pixel_offset, image_stride);
 std::uint32_t max_instances = 0U;
 for (const auto& image : request.split.images) max_instances = std::max<std::uint32_t>(max_instances, image.label_count);
 const FileHeader header = make_file_header({image_count, request.resolution, request.resolution, 3U, max_instances, image_stride, request.resize_mode}, class_catalog.names(), layout);
 validate_compiled_header(header);
 validate_compiled_index_entries(index, header, request.split.labels.size(), request.cancel_requested);
 validate_compiled_original_image_dimensions(index, request.cancel_requested);
 validate_compiled_annotation_provenance(index, request.split.labels, request.cancel_requested);
 const std::size_t used_rle = validate_compiled_label_entries(request.split.labels, header, layout.rle_block_size, request.cancel_requested);
 if (used_rle != layout.rle_block_size) { throw std::runtime_error("benchmark labels do not reference the complete mask block"); }
 validate_compiled_rle_pairs(request.split.labels, request.split.rle_pairs, static_cast<std::size_t>(request.resolution) * request.resolution, request.cancel_requested);
 auto& output = state.staging.file();
 const auto& staging_path = state.staging.path();
 if (slots.size() != state.images.size()) {
  // Destination always precedes source, including the exact final index prefix.
  // One fixed-size scratch handles even a single very large image safely.
  std::vector<std::uint8_t> buffer(std::min<std::size_t>(state.stride, 1024U * 1024U));
  for (std::size_t i = 0; i < slots.size(); ++i) {
   const auto source = state.layout.pixel_offset + slots[i] * state.stride;
   const auto destination = layout.pixel_offset + i * state.stride;
   if (destination > source) throw std::logic_error("benchmark compaction is not forward");
   if (destination == source) continue;
   for (std::size_t offset = 0; offset < state.stride;) {
    throw_if_benchmark_cancelled(request.cancel_requested);
    const auto bytes = std::min(buffer.size(), state.stride - offset);
    output.pread_all(buffer.data(), bytes, source + offset);
    output.pwrite_all(buffer.data(), bytes, destination + offset);
    offset += bytes;
   }
  }
 }
 state.pixels.reset();
 { const std::lock_guard lock(state.directory_mutex); state.directory = {}; state.directory_source = std::numeric_limits<std::uint16_t>::max(); }
 state.staging.resize(layout.total_size, "additional benchmark metadata staging");
 output.pwrite_all(&header, sizeof(header), 0U);
 output.pwrite_all(index.data(), layout.index_size, layout.index_offset);
 output.pwrite_all(request.split.labels.data(), layout.label_block_size, layout.label_offset);
 output.pwrite_all(request.split.rle_pairs.data(), layout.rle_block_size, layout.rle_offset);
 output.sync_data();
 state.staging.close();
 const common_io::FileHandle staged = common_io::FileHandle::open_readonly(staging_path.string());
 const FileHeader staged_header = read_compiled_header(staged);
 const CompiledFileSections sections = validate_compiled_file_sections(staged_header, staged.size());
 if (sections.label_count != request.split.labels.size() || sections.rle_region_bytes != layout.rle_block_size) {
  throw std::runtime_error("staged benchmark split metadata does not match its compile plan");
 }
 const ReadOnlyMappedRange persisted_index(staged, sections.index_offset, sections.expected_index_bytes);
 const auto persisted_index_span = std::span(reinterpret_cast<const ImageEntry*>(persisted_index.data()), static_cast<std::size_t>(staged_header.num_images));
 validate_compiled_index_entries(persisted_index_span, staged_header, sections.label_count, request.cancel_requested);
 validate_compiled_original_image_dimensions(persisted_index_span, request.cancel_requested);
 const ReadOnlyMappedRange persisted_labels(staged, sections.label_offset, sections.label_bytes);
 const auto persisted_label_span = std::span(reinterpret_cast<const PackedInstance*>(persisted_labels.data()), sections.label_count);
 const std::size_t persisted_rle_bytes = validate_compiled_label_entries(persisted_label_span, staged_header, sections.rle_region_bytes, request.cancel_requested);
 validate_compiled_annotation_provenance(persisted_index_span, persisted_label_span, request.cancel_requested);
 if (persisted_rle_bytes != layout.rle_block_size) { throw std::runtime_error("persisted benchmark labels do not reference the complete mask block"); }
 const ReadOnlyMappedRange persisted_rle(staged, sections.rle_offset, sections.rle_region_bytes);
 const auto persisted_rle_span = std::span(reinterpret_cast<const RLEPair*>(persisted_rle.data()), request.split.rle_pairs.size());
 validate_compiled_rle_pairs(persisted_label_span, persisted_rle_span, static_cast<std::size_t>(request.resolution) * request.resolution, request.cancel_requested);
 throw_if_benchmark_cancelled(request.cancel_requested);
 state.staging.publish(request.output_path, request.cancel_requested, BenchmarkStagedArtifact::Publication::DurableReplace, request.overwrite);
}
void write_benchmark_split(const BenchmarkWriteRequest& request) {
 for (const auto& image : request.split.images)
  if (!image.source_width || !image.source_height || image.source_index >= request.split.sources.size()) throw std::runtime_error("benchmark image metadata is incomplete");
 BenchmarkSplitWriter writer(request);
 writer.write_remaining(request);
 writer.finish(request);
}
}  // namespace mmltk::backend::data::benchmark_internal
