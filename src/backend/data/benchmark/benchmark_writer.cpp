#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
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
#include <iterator>
#include <ranges>
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
#include <deque>
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
using detail::WritablePixelRange;
[[nodiscard]] ImageEntry benchmark_image_entry(const EncodedImageRecord& image) {
 ImageEntry entry{};
 entry.status = ImageStatus::Eligible;
 entry.num_instances = image.label_count;
 entry.has_source_image_id = 1;
 entry.source_image_id = image.source_image_id;
 entry.source = image.annotation_source;
 entry.original_width = image.source_width;
 entry.original_height = image.source_height;
 return entry;
}
}  // namespace
struct BenchmarkPixelInput {
 BenchmarkSourcePublication publication;
 std::shared_ptr<const BenchmarkEncodedImage> payload;
 BenchmarkPixelInput(BenchmarkSourcePublication source, std::shared_ptr<const BenchmarkEncodedImage> image) : publication(std::move(source)), payload(std::move(image)) {}
};
struct BenchmarkSplitWriter::Impl {
 struct Scratch {
  BenchmarkImageDecoder decoder;
  mmltk::backend::imaging::resample::RgbImageResizer resizer;
  std::vector<std::uint8_t> decoded, cmyk;
  explicit Scratch(bool perceptual) : resizer(1, perceptual) {}
 };
 std::deque<EncodedImageRecord> images;
 std::vector<CachedImageSource> sources;
 std::deque<std::uint8_t> complete, header_known, pixel_allocated, label_pages;
 std::deque<ImageEntry> entries;
 std::deque<std::filesystem::path> input_roots;
 mutable std::mutex facts_mutex;
 std::vector<std::unique_ptr<Scratch>> scratch;
 Scratch& lane_scratch(std::size_t lane) {
  auto& value = scratch.at(lane);
  if (!value) value = std::make_unique<Scratch>(perceptual);
  return *value;
 }
 BenchmarkCompilePipeline* execution = nullptr;
 BenchmarkAllowance writer_handles, directory_handles;
 std::mutex directory_mutex;
 common_io::FileHandle directory;
 std::uint16_t directory_source = std::numeric_limits<std::uint16_t>::max();
 std::filesystem::path directory_root;
 BenchmarkStagedArtifact staging;
 std::unique_ptr<WritablePixelRange> pixels;
 FileLayout layout;
 std::uint32_t resolution;
 std::vector<std::string> class_names;
 mmltk::backend::imaging::resample::ImageResizeMode resize_mode;
 common_concurrency::CancellationObservation cancellation;
 BenchmarkWriteProgressEvent progress;
 BenchmarkImageReadObserver image_opened;
 bool actual_dimensions;
 bool perceptual;
 std::size_t stride;
 Impl(const BenchmarkWriteRequest& request, bool actual)
     : images(request.split.images.begin(), request.split.images.end()),
       sources(request.split.sources),
       complete(images.size()),
       header_known(images.size()),
       pixel_allocated(images.size()),
       label_pages(images.size()),
       resolution(request.resolution),
       class_names(request.split.class_names),
       resize_mode(request.resize_mode),
       cancellation(request.cancel_requested),
       progress(request.progress),
       image_opened(request.image_opened),
       actual_dimensions(actual),
       perceptual(request.perceptual_downscale),
       stride(common_math::checked_cast<std::size_t>(static_cast<std::uint64_t>(resolution) * resolution * 3U * sizeof(float), "benchmark image stride overflow")) {
  (void)catalog::ClassCatalog(class_names, COMPILED_CLASS_NAME_CAPACITY);
  if (class_names.empty() || class_names.size() > MAX_CLASSES) throw std::runtime_error("benchmark split exceeds the compiled class limit");
  execution = request.execution;
  if (execution) writer_handles = execution->reserve(BenchmarkResources::handles(1));
  if (resolution == 0 || resolution > MAX_IMAGE_EXTENT) throw std::runtime_error("benchmark pixel membership is incomplete");
  layout = compute_file_layout(common_math::checked_cast<std::uint32_t>(images.size(), "benchmark image count overflow"), stride);
  (void)common_io::ensure_parent_directory(request.output_path);
  StorageReservationPool storage(request.output_path, {}, execution ? &execution->storage() : nullptr);
  staging = BenchmarkStagedArtifact::create(storage, request.output_path, 0, "benchmark sparse staging");
  // The resident index is written only at seal. Keep the staging EOF aligned
  // so growth cannot allocate a partial EOF page in the next pixel block.
  staging.resize_sparse(layout.index_offset);
  pixels = std::make_unique<WritablePixelRange>(staging.file().get(), layout);
  entries.resize(images.size());
  input_roots.resize(images.size());
  const auto lanes = std::max(1, request.num_workers);
  scratch.reserve(lanes);
  for (int i = 0; i < lanes; ++i) scratch.push_back(std::make_unique<Scratch>(request.perceptual_downscale));
 }
 [[nodiscard]] std::vector<std::size_t> slots(const PreparedBenchmarkSplit& split, bool finished = false) const {
  std::vector<std::size_t> result;
  result.reserve(split.images.size());
  for (const auto& image : split.images) {
   std::optional<std::uint32_t> bound;
   const auto& root = split.sources.at(image.source_index).root;
   if (execution) bound = execution->image_slot(root, image.source_image_id);
   const std::lock_guard lock(facts_mutex);
   if (!bound) {
    for (std::size_t i = 0; i < images.size(); ++i)
     if (images[i].source_image_id == image.source_image_id && sources.at(images[i].source_index).root == root) {
      bound = static_cast<std::uint32_t>(i);
      break;
     }
   }
   if (!bound) throw std::runtime_error("benchmark final image is absent");
   const auto slot = *bound;
   if (finished) {
    if (!complete[slot]) throw std::runtime_error("benchmark publication has unfinished pixels");
    if (std::pair{images[slot].source_width, images[slot].source_height} != std::pair{image.source_width, image.source_height})
     throw std::runtime_error("benchmark labels and pixels disagree on source dimensions");
   }
   result.push_back(slot);
  }
  return result;
 }
};
BenchmarkSplitWriter::BenchmarkSplitWriter(const BenchmarkWriteRequest& request, bool actual) : impl_(std::make_unique<Impl>(request, actual)) {}
BenchmarkSplitWriter::~BenchmarkSplitWriter() = default;
std::pair<std::filesystem::path, std::uint64_t> BenchmarkSplitWriter::identity(std::size_t slot) const {
 const std::lock_guard lock(impl_->facts_mutex);
 const auto& image = impl_->images.at(slot);
 return {impl_->sources.at(image.source_index).root, image.source_image_id};
}
std::uint32_t BenchmarkSplitWriter::allocate_slot(EncodedImageRecord image, const CachedImageSource& source) {
 auto& state = *impl_;
 const std::lock_guard lock(state.facts_mutex);
 const auto slot = common_math::checked_cast<std::uint32_t>(state.images.size(), "benchmark slot overflow");
 const auto layout = compute_file_layout(common_math::checked_add(slot, 1U, "benchmark slot overflow"), state.stride);
 auto found = std::ranges::find(state.sources, source.root, &CachedImageSource::root);
 if (found == state.sources.end()) {
  image.source_index = common_math::checked_cast<std::uint16_t>(state.sources.size(), "benchmark source count overflow");
  state.sources.push_back(source);
 } else
  image.source_index = common_math::checked_cast<std::uint16_t>(std::distance(state.sources.begin(), found), "benchmark source index overflow");
 if (layout.index_offset != state.layout.index_offset) state.staging.resize_sparse(layout.index_offset);
 state.pixels->grow(layout);
 state.images.push_back(image);
 state.complete.push_back(0);
 state.header_known.push_back(0);
 state.pixel_allocated.push_back(0);
 state.label_pages.push_back(0);
 state.entries.emplace_back();
 state.input_roots.emplace_back();
 state.layout = layout;
 return slot;
}
void BenchmarkSplitWriter::input_root(std::uint32_t slot, const std::filesystem::path& root) {
 const std::lock_guard lock(impl_->facts_mutex);
 impl_->input_roots.at(slot) = root;
}
ImageEntry BenchmarkSplitWriter::entry(std::uint32_t slot) const {
 const std::lock_guard lock(impl_->facts_mutex);
 return impl_->entries.at(slot);
}
void BenchmarkSplitWriter::invalidate_labels(std::uint32_t slot) {
 const std::lock_guard lock(impl_->facts_mutex);
 impl_->entries.at(slot) = {};
}
void BenchmarkSplitWriter::settle_labels(std::uint32_t slot, std::uint64_t ordinal_base, bool ordinal_incomplete) {
 if (!ordinal_base && !ordinal_incomplete) return;
 const std::lock_guard lock(impl_->facts_mutex);
 const auto& entry = impl_->entries.at(slot);
 write_compiled_label_ordinals(impl_->staging.file(), impl_->layout.label_offset(slot), entry.num_instances, {}, ordinal_base, ordinal_incomplete);
}
void BenchmarkSplitWriter::write_labels(std::uint32_t slot, ImageEntry entry, ImageAnnotations annotations, std::size_t lane, std::uint64_t ordinal_base) {
 auto& state = *impl_;
 throw_if_benchmark_cancelled(state.cancellation);
 entry.status = ImageStatus::Eligible;
 set_annotation_extent(entry, annotations.labels.size(), annotations.runs.size(), std::to_string(entry.source_image_id));
 FileHeader checks{};
 checks.num_classes = common_math::checked_cast<std::uint32_t>(state.class_names.size(), "benchmark class count overflow");
 checks.max_instances_per_image = UINT16_MAX;
 checks.image_width = checks.image_height = state.resolution;
 (void)admit_compiled_annotations(entry, annotations, checks, slot, state.cancellation);
 (void)lane;
 FileLayout layout;
 {
  const std::lock_guard lock(state.facts_mutex);
  const auto& image = state.images.at(slot);
  if (image.source_image_id != entry.source_image_id) throw std::runtime_error("benchmark label identity or canvas differs from pixels");
  layout = state.layout;
  const auto offset = layout.label_offset(slot);
  const auto pages = compiled_align_up(entry.label_bytes, PAGE_SIZE) / PAGE_SIZE;
  const auto retained = state.label_pages[slot];
  if (pages > retained) {
   state.staging.allocate_range(offset + retained * PAGE_SIZE, (pages - retained) * PAGE_SIZE);
   state.label_pages[slot] = common_math::checked_cast<std::uint8_t>(pages, "benchmark label page count overflow");
  }
 }
 write_compiled_annotations(state.staging.file(), layout, slot, entry, annotations, ordinal_base);
 {
  const std::lock_guard lock(state.facts_mutex);
  state.entries[slot] = entry;
 }
}
void BenchmarkSplitWriter::retire_scratch(std::size_t lane) noexcept {
 impl_->scratch[lane].reset();
 const std::lock_guard lock(impl_->directory_mutex);
 impl_->directory = {};
 impl_->directory_source = std::numeric_limits<std::uint16_t>::max();
 impl_->directory_root.clear();
 impl_->directory_handles = {};
}
void BenchmarkSplitWriter::prepare_lanes(std::size_t lanes) {
 while (impl_->scratch.size() < lanes) impl_->scratch.push_back(std::make_unique<Impl::Scratch>(impl_->perceptual));
}
std::shared_ptr<BenchmarkPixelInput> BenchmarkSplitWriter::prepare_pixel(
 std::size_t slot, std::size_t lane, BenchmarkSourcePublication publication, BenchmarkAllowance allowance, std::shared_ptr<const BenchmarkEncodedImage> payload) {
 auto& state = *impl_;
 EncodedImageRecord image;
 std::filesystem::path source_root;
 {
  const std::lock_guard lock(state.facts_mutex);
  image = state.images.at(slot);
  source_root = state.input_roots.at(slot).empty() ? state.sources.at(image.source_index).root : state.input_roots.at(slot);
 }
 if (image_complete(slot)) return {};
 throw_if_benchmark_cancelled(state.cancellation);
 auto& scratch = state.lane_scratch(lane);
 auto input = std::make_shared<BenchmarkPixelInput>(std::move(publication), std::move(payload));
 try {
  if (!input->payload || input->payload->encoded().empty()) {
   common_io::FileHandle file;
   {
    const std::lock_guard lock(state.directory_mutex);
    if (state.directory_root != source_root) {
     state.directory = {};
     state.directory_source = std::numeric_limits<std::uint16_t>::max();
     state.directory_handles = {};
     if (state.execution) {
      auto granted = state.execution->try_reserve(BenchmarkResources::handles(1), allowance);
      if (!granted) throw std::logic_error("benchmark image input lost its directory continuation");
      state.directory_handles = std::move(*granted);
     }
     const auto& root = source_root;
     const int descriptor = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
     if (descriptor < 0) throw common_io::errno_error("cannot open cached benchmark image directory", root.string());
     state.directory = common_io::FileHandle(descriptor);
     state.directory_source = image.source_index;
     state.directory_root = source_root;
    }
    std::array<char, 24> relative{};
    (void)format_cached_image_relative_path(image.source_image_id, relative);
    const int descriptor = ::openat(state.directory.get(), relative.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
     if (errno == ENOENT) throw BenchmarkImageError("cached benchmark image is missing");
     throw common_io::errno_error("cannot open cached benchmark image", relative.data());
    }
    file = common_io::FileHandle(descriptor);
   }
   const CachedImageValidator header = [&](std::uint64_t, std::span<const std::uint8_t> encoded) {
    return scratch.decoder.read_header(encoded, state.actual_dimensions ? 0 : image.source_width, state.actual_dimensions ? 0 : image.source_height);
   };
   input->payload = BenchmarkEncodedImage::open(std::move(file), image.source_image_id, header, state.cancellation, nullptr, allowance, input->payload, std::numeric_limits<std::uint32_t>::max());
   if (!input->payload) throw BenchmarkImageError("cached benchmark image is missing or has an invalid size");
  }
  const auto& header = input->payload->header();
  if (!state.actual_dimensions && ((image.source_width && image.source_width != header.width) || (image.source_height && image.source_height != header.height)))
   throw BenchmarkImageError("benchmark image dimensions do not match annotations");
  if (state.image_opened) state.image_opened(source_root, image.source_image_id);
  {
   const std::lock_guard lock(state.facts_mutex);
   state.images[slot].source_width = header.width;
   state.images[slot].source_height = header.height;
   state.header_known[slot] = 1;
  }
  // The mapping owns its inode after the local image file closes. The cached
  // directory owns its separate child grant until reuse or scratch retirement.
  allowance.retire_descriptors();
 } catch (const std::exception& error) {
  if (!dynamic_cast<const BenchmarkImageError*>(&error) && !dynamic_cast<const InvalidImageError*>(&error)) throw;
  throw_if_benchmark_cancelled(state.cancellation);
  throw BenchmarkImageReadError(image.source_index, image.source_image_id, error.what());
 }
 return input;
}
BenchmarkAllowance BenchmarkSplitWriter::pixel_input_allowance(const BenchmarkPixelInput& input) const { return input.payload->allowance(); }
std::uint64_t BenchmarkSplitWriter::pixel_workspace_bytes(const BenchmarkPixelInput& input) const {
 return pixel_workspace_bytes(input.payload->header(), input.payload->charged_bytes() ? 0 : input.payload->size());
}
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
 } catch (const BenchmarkImageError& error) {
  throw_if_benchmark_cancelled(state.cancellation);
  const std::lock_guard lock(state.facts_mutex);
  const auto& image = state.images.at(slot);
  throw BenchmarkImageReadError(image.source_index, image.source_image_id, error.what());
 }
 {
  const std::lock_guard lock(state.facts_mutex);
  if (!state.pixel_allocated[slot]) {
   state.staging.allocate_range(state.layout.pixel_offset(common_math::checked_cast<std::uint32_t>(slot, "benchmark pixel slot overflow")), state.layout.pixel_pitch);
   state.pixel_allocated[slot] = 1;
  }
 }
 scratch.resizer.resize_to_planar(
  {decoded.data(), {header.width, header.height, static_cast<std::size_t>(header.width) * 3U, 0U, decoded.size(), mmltk::backend::imaging::resample::RgbPixelFormat::RGB8}},
  {state.pixels->image(common_math::checked_cast<std::uint32_t>(slot, "benchmark pixel slot overflow")),
   {state.resolution, state.resolution, static_cast<std::size_t>(state.resolution) * sizeof(float), static_cast<std::size_t>(state.resolution) * state.resolution * sizeof(float), state.stride,
    mmltk::backend::imaging::resample::RgbPixelFormat::PlanarUnitSrgbF32}},
  state.resize_mode);
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
bool BenchmarkSplitWriter::image_complete(std::size_t slot) const {
 const std::lock_guard lock(impl_->facts_mutex);
 return impl_->complete.at(slot) != 0;
}
std::pair<std::uint32_t, std::uint32_t> BenchmarkSplitWriter::dimensions(std::size_t slot) const {
 const std::lock_guard lock(impl_->facts_mutex);
 if (!impl_->complete.at(slot)) throw std::logic_error("benchmark image dimensions requested before pixel completion");
 const auto& image = impl_->images.at(slot);
 return {image.source_width, image.source_height};
}
std::size_t BenchmarkSplitWriter::completed() const noexcept {
 const std::lock_guard lock(impl_->facts_mutex);
 return std::ranges::count(impl_->complete, std::uint8_t{1});
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
  const std::lock_guard facts(impl_->facts_mutex);
  if (impl_->directory.get() >= 0 && impl_->sources.at(impl_->directory_source).root == root) {
   impl_->directory = {};
   impl_->directory_source = std::numeric_limits<std::uint16_t>::max();
   impl_->directory_root.clear();
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
void BenchmarkSplitWriter::write_remaining(const BenchmarkWriteRequest& request) {
 {
  const std::lock_guard lock(impl_->facts_mutex);
  impl_->progress = request.progress;
 }
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
BenchmarkSealedSplit BenchmarkSplitWriter::seal(const BenchmarkWriteRequest& request) {
 auto& state = *impl_;
 if (request.split.images.empty()) throw std::runtime_error("benchmark final annotations are incomplete");
 const auto slots = state.slots(request.split, true);
 for (std::size_t i = 0; i < slots.size(); ++i) {
  throw_if_compiled_validation_cancelled(i, request.cancel_requested);
  const auto physical_slot = common_math::checked_cast<std::uint32_t>(slots[i], "benchmark slot overflow");
  const auto expected = benchmark_image_entry(request.split.images[i]);
  if (!state.entries[physical_slot].eligible()) {
   if (expected.num_instances) throw std::runtime_error("benchmark final label payload missing");
   write_labels(physical_slot, expected, {});
  }
  const auto entry = state.entries[physical_slot];
  if (entry.num_instances != expected.num_instances || entry.original_width != expected.original_width || entry.original_height != expected.original_height || entry.source != expected.source ||
      entry.has_source_image_id != expected.has_source_image_id || entry.source_image_id != expected.source_image_id)
   throw std::runtime_error("benchmark final annotation facts differ from the published slot");
 }
 std::vector<std::uint32_t> selected(slots.begin(), slots.end());
 return seal_slots(request, selected);
}
BenchmarkSealedSplit BenchmarkSplitWriter::seal_slots(const BenchmarkWriteRequest& request, std::span<const std::uint32_t> slots) {
 auto& state = *impl_;
 if (request.resolution != state.resolution || request.resize_mode != state.resize_mode || request.split.class_names != state.class_names)
  throw std::runtime_error("benchmark final geometry or class catalog changed");
 if (slots.empty()) throw std::runtime_error("required benchmark split contains no healthy images: " + request.split.name);
 std::vector<ImageEntry> index(state.images.size());
 std::uint32_t maximum = 0;
 auto class_catalog = std::make_shared<const catalog::ClassCatalog>(state.class_names, COMPILED_CLASS_NAME_CAPACITY);
 for (const auto slot : slots) {
  throw_if_compiled_validation_cancelled(slot, request.cancel_requested);
  const auto& entry = state.entries.at(slot);
  const auto& image = state.images.at(slot);
  if (index[slot].eligible() || !state.complete[slot] || !entry.eligible() || entry.original_width != image.source_width || entry.original_height != image.source_height)
   throw std::runtime_error("benchmark publication has unfinished or contradictory sample " + std::to_string(image.source_image_id));
  index[slot] = entry;
  maximum = std::max<std::uint32_t>(maximum, entry.num_instances);
 }
 state.pixels.reset();
 {
  const std::lock_guard lock(state.directory_mutex);
  state.directory = {};
  state.directory_source = std::numeric_limits<std::uint16_t>::max();
 }
 state.layout = compact_compiled_file(
  state.staging.file(), state.layout, index, request.cancel_requested, request.compaction, [&](std::size_t offset, std::size_t bytes) { state.staging.allocate_range(offset, bytes); });
 state.staging.reconcile();
 const auto header =
  make_file_header({common_math::checked_cast<std::uint32_t>(slots.size(), "benchmark image count overflow"), state.resolution, state.resolution, 3U, maximum, state.stride, state.resize_mode},
   class_catalog->names(), state.layout);
 state.staging.allocate_range(0, compiled_align_up(sizeof(header), PAGE_SIZE));
 state.staging.allocate_range(state.layout.index_offset, compiled_align_up(state.layout.index_size, PAGE_SIZE));
 auto& output = state.staging.file();
 output.pwrite_all(&header, sizeof(header), 0);
 output.pwrite_all(index.data(), state.layout.index_size, state.layout.index_offset);
 state.staging.resize_sparse(state.layout.total_size);
 output.sync_data();
 const auto persisted = read_compiled_header(output);
 (void)validate_compiled_file_sections(persisted, output.size());
 if (std::memcmp(&persisted, &header, sizeof(header))) throw std::runtime_error("staged benchmark header differs");
 state.staging.close();
 throw_if_benchmark_cancelled(request.cancel_requested);
 CompiledDatasetInfo info{request.output_path, header.num_images, header.image_width, header.image_height, header.channels, header.max_instances_per_image, std::move(class_catalog)};
 return {std::move(state.staging), std::move(info)};
}
void BenchmarkSplitWriter::finish(const BenchmarkWriteRequest& request) {
 auto sealed = seal(request);
 sealed.artifact.publish(request.output_path, request.cancel_requested, BenchmarkStagedArtifact::Publication::DurableReplace, request.overwrite);
}
void write_benchmark_split(const BenchmarkWriteRequest& request) {
 for (const auto& image : request.split.images)
  if (!image.source_width || !image.source_height || image.source_index >= request.split.sources.size()) throw std::runtime_error("benchmark image metadata is incomplete");
 BenchmarkSplitWriter writer(request);
 writer.write_remaining(request);
 writer.finish(request);
}
}  // namespace mmltk::backend::data::benchmark_internal
