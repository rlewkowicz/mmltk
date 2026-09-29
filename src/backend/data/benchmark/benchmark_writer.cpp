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
[[nodiscard]] ImageEntry benchmark_image_entry(const EncodedImageRecord& image) {
 ImageEntry entry{};
 entry.label_offset = common_math::checked_cast<std::uint32_t>(std::size_t{image.first_label} * sizeof(PackedInstance), "benchmark label offset overflow");
 entry.num_instances = image.label_count;
 entry.label_bytes = std::uint32_t{image.label_count} * sizeof(PackedInstance);
 entry.has_source_image_id = 1;
 entry.source_image_id = image.source_image_id;
 entry.source = image.annotation_source;
 entry.original_width = image.source_width;
 entry.original_height = image.source_height;
 return entry;
}
}  // namespace
BenchmarkSplitAssembly::BenchmarkSplitAssembly(std::string name, std::span<const std::string_view> classes, std::uint32_t resolution, mmltk::backend::imaging::resample::ImageResizeMode resize)
    : resolution_(resolution), resize_mode_(resize) {
 split_.name = std::move(name);
 for (const auto value : classes) split_.class_names.emplace_back(value);
 if (!resolution || resolution > MAX_IMAGE_EXTENT) throw std::runtime_error("benchmark resolution exceeds the compiled coordinate format");
 catalog_ = std::make_shared<const catalog::ClassCatalog>(split_.class_names, COMPILED_CLASS_NAME_CAPACITY);
 if (split_.class_names.empty() || split_.class_names.size() > MAX_CLASSES) throw std::runtime_error("benchmark split exceeds the compiled class limit");
}
void BenchmarkSplitAssembly::mutable_records() const {
 if (placement_started_ || sealed_) throw std::logic_error("benchmark metadata membership already placed");
}
void BenchmarkSplitAssembly::add_source(std::filesystem::path root) {
 mutable_records();
 if (split_.sources.size() >= UINT16_MAX) throw std::runtime_error("benchmark split exceeds its cached source representation");
 split_.sources.push_back({std::move(root)});
}
void BenchmarkSplitAssembly::image(EncodedImageRecord image, std::shared_ptr<const BenchmarkLabelChunk> chunk, std::uint64_t ordinal_base) {
 mutable_records();
 if (!chunk || image.source_index >= split_.sources.size() || image.label_count != chunk->labels.size() || image.source_width != chunk->width || image.source_height != chunk->height)
  throw std::runtime_error("benchmark image assembly extent is invalid");
 image.first_label = 0;  // Only the complete split assigns canonical global prefixes.
 const auto entry = benchmark_image_entry(image);
 CompiledRecordChecks::image(entry, split_.images.size());
 const auto labels = checked_size_add(labels_, chunk->labels.size(), "benchmark label count overflow");
 const auto runs = checked_size_add(runs_, chunk->runs.size(), "benchmark run count overflow");
 split_.images.push_back(image);
 index_.push_back(entry);
 placements_.push_back({std::move(chunk), ordinal_base});
 labels_ = labels;
 runs_ = runs;
}
void BenchmarkSplitAssembly::append(BenchmarkSplitAssembly&& source) {
 mutable_records();
 source.mutable_records();
 if (resolution_ != source.resolution_ || resize_mode_ != source.resize_mode_ || split_.class_names != source.split_.class_names) throw std::logic_error("benchmark metadata merge changed format");
 if (split_.images.empty() && split_.sources.empty()) {
  const auto name = split_.name;
  *this = std::move(source);
  split_.name = name;
  source = {};
  return;
 }
 const auto images = checked_size_add(split_.images.size(), source.split_.images.size(), "benchmark image count overflow");
 const auto labels = checked_size_add(labels_, source.labels_, "benchmark label count overflow");
 const auto runs = checked_size_add(runs_, source.runs_, "benchmark run count overflow");
 const auto source_base = split_.sources.size();
 if (source_base + source.split_.sources.size() > UINT16_MAX) throw std::runtime_error("benchmark cached source index overflow");
 split_.images.reserve(images);
 index_.reserve(images);
 placements_.reserve(images);
 for (auto image : source.split_.images) {
  image.source_index = common_math::checked_cast<std::uint16_t>(image.source_index + source_base, "benchmark source index overflow");
  split_.images.push_back(image);
 }
 index_.insert(index_.end(), source.index_.begin(), source.index_.end());
 placements_.insert(placements_.end(), std::make_move_iterator(source.placements_.begin()), std::make_move_iterator(source.placements_.end()));
 split_.sources.insert(split_.sources.end(), std::make_move_iterator(source.split_.sources.begin()), std::make_move_iterator(source.split_.sources.end()));
 labels_ = labels;
 runs_ = runs;
 source = {};
}
void BenchmarkSplitAssembly::materialize(BenchmarkCompilePipeline& execution, common_concurrency::CancellationObservation cancellation) {
 mutable_records();
 placement_started_ = true;
 const auto stride = std::size_t{resolution_} * resolution_ * 3U * sizeof(float);
 layout_ = compute_pixel_layout(common_math::checked_cast<std::uint32_t>(split_.images.size(), "benchmark image count overflow"), stride);
 finalize_layout(layout_, {labels_, runs_});
 split_.labels.resize(labels_);
 split_.rle_pairs.resize(runs_);
 std::size_t labels = 0, runs = 0;
 for (std::size_t i = 0; i < placements_.size(); ++i) {
  throw_if_compiled_validation_cancelled(i, cancellation);
  auto& image = split_.images[i];
  auto& placement = placements_[i];
  image.first_label = common_math::checked_cast<std::uint32_t>(labels, "benchmark label index overflow");
  index_[i].label_offset = common_math::checked_cast<std::uint32_t>(checked_size_multiply(labels, sizeof(PackedInstance), "benchmark label offset overflow"), "benchmark label offset overflow");
  index_[i].pixel_offset = layout_.pixel_offset + i * stride;
  placement.first_run = runs;
  labels = checked_size_add(labels, image.label_count, "benchmark label count overflow");
  runs = checked_size_add(runs, placement.chunk->runs.size(), "benchmark run count overflow");
  maximum_ = std::max<std::uint32_t>(maximum_, image.label_count);
 }
 if (labels != labels_ || runs != runs_) throw std::runtime_error("benchmark final metadata coverage is incomplete");
 // Complete fixed destinations precede submission. for_each settles every
 // borrower on success/failure, so rebuilding cannot race a placement write.
 execution.for_each(BenchmarkStage::Labels, placements_.size(), BenchmarkResources{}, [&](std::size_t slot) {
  auto& placement = placements_[slot];
  const auto& image = split_.images[slot];
  const auto& product = *placement.chunk;
  const auto offset = checked_size_multiply(placement.first_run, sizeof(RLEPair), "benchmark mask offset overflow");
  const auto bytes = checked_size_multiply(product.runs.size(), sizeof(RLEPair), "benchmark mask size overflow");
  const auto pixels = std::size_t{resolution_} * resolution_;
  std::size_t consumed = 0;
  for (std::size_t i = 0; i < product.labels.size(); ++i) {
   throw_if_compiled_validation_cancelled(i, cancellation);
   auto value = product.labels[i];
   const auto end = CompiledRecordChecks::label(value, static_cast<std::uint32_t>(split_.class_names.size()), bytes, consumed, image.annotation_source, image.first_label + i);
   const auto begin = consumed;
   std::size_t previous_end = 0;
   for (; consumed < end; consumed += sizeof(RLEPair)) {
    const auto position = consumed / sizeof(RLEPair);
    throw_if_compiled_validation_cancelled(position, cancellation);
    const auto run = product.runs[position];
    previous_end = CompiledRecordChecks::run(run, previous_end, pixels, image.first_label + i, (consumed - begin) / sizeof(RLEPair));
    split_.rle_pairs[placement.first_run + position] = run;
   }
   value.source_ordinal = common_math::checked_add(value.source_ordinal, placement.ordinal_base, "benchmark source ordinal overflow");
   value.mask_rle_offset = common_math::checked_add(value.mask_rle_offset, offset, "benchmark mask offset overflow");
   split_.labels[image.first_label + i] = value;
  }
  if (consumed != bytes) throw std::runtime_error("benchmark labels do not reference the complete mask block");
  // The pipeline keeps its independent generation-bound reuse ownership.
  placement.chunk.reset();
 });
 throw_if_compiled_validation_cancelled(0, cancellation);
 materialized_ = true;
}
void BenchmarkSplitAssembly::finish(common_concurrency::CancellationObservation cancellation) {
 if (sealed_) return;
 throw_if_compiled_validation_cancelled(0, cancellation);
 if (!materialized_) throw std::logic_error("benchmark metadata placement has not completed");
 if (split_.images.empty() || split_.sources.empty() || !catalog_) throw std::runtime_error("benchmark split must contain images, sources and classes");
 const auto stride = std::size_t{resolution_} * resolution_ * 3U * sizeof(float);
 const auto count = common_math::checked_cast<std::uint32_t>(split_.images.size(), "benchmark image count overflow");
 header_ = make_file_header({count, resolution_, resolution_, 3U, maximum_, stride, resize_mode_}, catalog_->names(), layout_);
 validate_compiled_header(header_);
 sealed_ = true;
}
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
 BenchmarkAllowance writer_handles, directory_handles;
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
  if (execution) writer_handles = execution->reserve(BenchmarkResources::handles(1));
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
 [[nodiscard]] std::vector<std::size_t> slots(const PreparedBenchmarkSplit& split, bool finished = false) const {
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
   if (finished) {
    if (!complete[next]) throw std::runtime_error("benchmark publication has unfinished pixels");
    if (std::pair{images[next].source_width, images[next].source_height} != std::pair{image.source_width, image.source_height})
     throw std::runtime_error("benchmark labels and pixels disagree on source dimensions");
   }
   result.push_back(next++);
  }
  return result;
 }
};
BenchmarkSplitWriter::BenchmarkSplitWriter(const BenchmarkWriteRequest& request, bool actual) : impl_(std::make_unique<Impl>(request, actual)) {}
BenchmarkSplitWriter::~BenchmarkSplitWriter() = default;
void BenchmarkSplitWriter::retire_scratch(std::size_t lane) noexcept {
 impl_->scratch[lane].reset();
 const std::lock_guard lock(impl_->directory_mutex);
 impl_->directory = {};
 impl_->directory_source = std::numeric_limits<std::uint16_t>::max();
 impl_->directory_handles = {};
}
void BenchmarkSplitWriter::prepare_lanes(std::size_t lanes) {
 while (impl_->scratch.size() < lanes) impl_->scratch.push_back(std::make_unique<Impl::Scratch>(impl_->perceptual));
}
std::shared_ptr<BenchmarkPixelInput> BenchmarkSplitWriter::prepare_pixel(
 std::size_t slot, std::size_t lane, BenchmarkSourcePublication publication, BenchmarkAllowance allowance, std::shared_ptr<const BenchmarkEncodedImage> payload) {
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
     state.directory_handles = {};
     if (state.execution) {
      auto granted = state.execution->try_reserve(BenchmarkResources::handles(1), allowance);
      if (!granted) throw std::logic_error("benchmark image input lost its directory continuation");
      state.directory_handles = std::move(*granted);
     }
     const auto& root = state.sources.at(image.source_index).root;
     const int descriptor = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
     if (descriptor < 0) throw common_io::errno_error("cannot open cached benchmark image directory", root.string());
     state.directory = common_io::FileHandle(descriptor);
     state.directory_source = image.source_index;
    }
    std::array<char, 24> relative{};
    (void)format_cached_image_relative_path(image.source_image_id, relative);
    const int descriptor = ::openat(state.directory.get(), relative.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) throw common_io::errno_error("cannot open cached benchmark image", relative.data());
    file = common_io::FileHandle(descriptor);
   }
   const CachedImageValidator header = [&](std::uint64_t, std::span<const std::uint8_t> encoded) {
    return scratch.decoder.read_header(encoded, state.actual_dimensions ? 0 : image.source_width, state.actual_dimensions ? 0 : image.source_height);
   };
   input->payload =
    BenchmarkEncodedImage::open(std::move(file), image.source_image_id, header, state.cancellation, nullptr, allowance, input->payload, std::numeric_limits<std::uint32_t>::max());
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
  // The mapping owns its inode after the local image file closes. The cached
  // directory owns its separate child grant until reuse or scratch retirement.
  allowance.retire_descriptors();
 } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) {
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
 } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) {
  throw_if_benchmark_cancelled(state.cancellation);
  const auto& image = state.images.at(slot);
  throw BenchmarkImageReadError(image.source_index, image.source_image_id, error.what());
 }
 scratch.resizer.resize_to_planar(
  {decoded.data(), {header.width, header.height, static_cast<std::size_t>(header.width) * 3U, 0U, decoded.size(), mmltk::backend::imaging::resample::RgbPixelFormat::RGB8}},
  {state.pixels->image(common_math::checked_cast<std::uint32_t>(slot, "benchmark pixel slot overflow"), state.stride),
   {state.resolution, state.resolution, static_cast<std::size_t>(state.resolution) * sizeof(float), static_cast<std::size_t>(state.resolution) * state.resolution * sizeof(float), state.stride,
    mmltk::backend::imaging::resample::RgbPixelFormat::PlanarUnitSrgbF32}},
  state.resize_mode);
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
BenchmarkSealedSplit BenchmarkSplitWriter::seal(const BenchmarkWriteRequest& request, BenchmarkSplitAssembly* product) {
 auto& state = *impl_;
 if (request.resolution != state.resolution || request.resize_mode != state.resize_mode) throw std::runtime_error("benchmark final pixel geometry changed");
 std::vector<std::size_t> slots;
 mmltk::common::logging::ScopedProfile profile{"benchmark.writer.total"};
 if (request.resolution == 0U || request.resolution > MAX_IMAGE_EXTENT) { throw std::runtime_error("benchmark resolution exceeds the compiled coordinate format"); }
 if (request.split.images.empty() || request.split.class_names.empty()) { throw std::runtime_error("benchmark split must contain images and classes"); }
 std::vector<ImageEntry> admitted_index;
 FileLayout layout;
 FileHeader header;
 std::span<const ImageEntry> index;
 std::shared_ptr<const catalog::ClassCatalog> class_catalog;
 const auto metadata = [&](std::size_t) {
  slots = state.slots(request.split, true);
  if (product) {
   if (&product->data() != &request.split || product->resolution_ != request.resolution || product->resize_mode_ != request.resize_mode)
    throw std::logic_error("benchmark sealed product does not own the requested records");
   product->finish(request.cancel_requested);
   layout = product->layout_;
   header = product->header_;
   index = product->index_;
   class_catalog = product->catalog_;
  } else {
   class_catalog = std::make_shared<const catalog::ClassCatalog>(request.split.class_names, COMPILED_CLASS_NAME_CAPACITY);
   if (request.split.class_names.size() > MAX_CLASSES || request.split.sources.empty()) throw std::runtime_error("benchmark split classes or sources are invalid");
   const auto stride = std::size_t{request.resolution} * request.resolution * 3U * sizeof(float);
   const auto count = common_math::checked_cast<std::uint32_t>(request.split.images.size(), "benchmark image count overflow");
   layout = compute_pixel_layout(count, stride);
   finalize_layout(layout, {request.split.labels.size(), request.split.rle_pairs.size()});
   admitted_index.reserve(count);
   std::uint32_t maximum = 0;
   header = make_file_header({count, request.resolution, request.resolution, 3U, UINT16_MAX, stride, request.resize_mode}, class_catalog->names(), layout);
   validate_compiled_header(header);
   const auto entries = std::views::iota(std::size_t{0}, request.split.images.size()) | std::views::transform([&](std::size_t slot) {
    auto entry = benchmark_image_entry(request.split.images[slot]);
    entry.pixel_offset = layout.pixel_offset + slot * stride;
    return entry;
   });
   // Caller-built requests carry no compiler seal. Construct their index and
   // maximum count in the same traversal that admits images, labels and runs.
   (void)admit_compiled_records(entries, request.split.labels, request.split.rle_pairs, header, [&](const ImageEntry& entry) {
    admitted_index.push_back(entry);
    maximum = std::max<std::uint32_t>(maximum, entry.num_instances);
   }, request.cancel_requested);
   header.max_instances_per_image = maximum;
   index = admitted_index;
  }
 };
 if (state.execution)
  state.execution->run(BenchmarkStage::Metadata, {}, metadata);
 else
  metadata(0);
 auto& output = state.staging.file();
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
 {
  const std::lock_guard lock(state.directory_mutex);
  state.directory = {};
  state.directory_source = std::numeric_limits<std::uint16_t>::max();
 }
 state.staging.resize(layout.total_size, "additional benchmark metadata staging");
 output.pwrite_all(&header, sizeof(header), 0U);
 output.pwrite_all(index.data(), layout.index_size, layout.index_offset);
 output.pwrite_all(request.split.labels.data(), layout.label_block_size, layout.label_offset);
 output.pwrite_all(request.split.rle_pairs.data(), layout.rle_block_size, layout.rle_offset);
 output.sync_data();
 // Retain the existing descriptor for persisted header/extent confirmation.
 // Successful writes and fdatasync establish durability; records were already
 // admitted by their producer (or the standalone entry above).
 const FileHeader persisted = read_compiled_header(output);
 const auto sections = validate_compiled_file_sections(persisted, output.size());
 if (std::memcmp(&persisted, &header, sizeof(header)) != 0 || sections.label_count != request.split.labels.size() || sections.rle_region_bytes != layout.rle_block_size)
  throw std::runtime_error("staged benchmark split metadata does not match its compile plan");
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
