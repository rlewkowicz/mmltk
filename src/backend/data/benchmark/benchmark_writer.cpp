#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/detail/staged_artifact.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/compile_pixel_workspace.h"
#include "src/backend/data/detail/compile_workspace.h"
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
#include "src/backend/data/detail/compiled_slot_storage.h"
#include "src/backend/data/detail/file_work_queue.h"
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
using mmltk::backend::data::detail::CompileAllowance;
using mmltk::backend::data::detail::CompileResources;
using mmltk::backend::data::detail::CompileTaskKind;
namespace common_concurrency = mmltk::common::concurrency;
namespace common_io = mmltk::common::io;
namespace common_math = mmltk::common::math;
namespace {
using detail::CompiledSlotStorage;
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
 std::shared_ptr<const detail::CompilePixelWorkspacePlan> workspace;
 BenchmarkPixelInput(BenchmarkSourcePublication source, std::shared_ptr<const BenchmarkEncodedImage> image) : publication(std::move(source)), payload(std::move(image)) {}
};
struct BenchmarkSplitWriter::Impl {
 std::deque<EncodedImageRecord> images;
 std::vector<CachedImageSource> sources;
 std::deque<std::uint8_t> complete, header_known;
 std::deque<ImageEntry> entries;
 std::deque<std::filesystem::path> input_roots;
 mutable std::mutex facts_mutex;
 BenchmarkCompilePipeline* execution = nullptr;
 std::unique_ptr<detail::CompileExecution> standalone_execution;
 std::unique_ptr<detail::FileWorkQueue> standalone_files;
 std::once_flag standalone_output;
 std::optional<int> output_cpu;
 detail::CompileExecution* output_execution = nullptr;
 detail::FileWorkQueue* files = nullptr;
 CompileAllowance writer_handles, directory_handles;
 std::mutex directory_mutex;
 common_io::FileHandle directory;
 std::uint16_t directory_source = std::numeric_limits<std::uint16_t>::max();
 std::filesystem::path directory_root;
 detail::StagedArtifact staging;
 std::unique_ptr<CompiledSlotStorage> slots_storage;
 FileLayout layout;
 std::uint32_t resolution;
 std::vector<std::string> class_names;
 mmltk::backend::imaging::resample::ImageResizeMode resize_mode;
 common_concurrency::CancellationObservation cancellation;
 BenchmarkWriteProgressEvent progress;
 BenchmarkTraceSink trace;
 BenchmarkImageReadObserver image_opened, pixel_preparing, extent_preparing;
 bool actual_dimensions;
 bool perceptual;
 std::size_t stride;
 Impl(const BenchmarkWriteRequest& request, bool actual)
     : images(request.split.images.begin(), request.split.images.end()),
       sources(request.split.sources),
       complete(images.size()),
       header_known(images.size()),
       resolution(request.resolution),
       class_names(request.split.class_names),
       resize_mode(request.resize_mode),
       cancellation(request.cancel_requested),
       progress(request.progress),
       image_opened(request.image_opened),
       pixel_preparing(request.pixel_preparing),
       extent_preparing(request.extent_preparing),
       actual_dimensions(actual),
       perceptual(request.perceptual_downscale),
       stride(common_math::checked_cast<std::size_t>(static_cast<std::uint64_t>(resolution) * resolution * 3U * sizeof(float), "benchmark image stride overflow")) {
  if (request.trace) try {
    trace = request.trace;
   } catch (...) {}
  (void)catalog::ClassCatalog(class_names, COMPILED_CLASS_NAME_CAPACITY);
  if (class_names.empty() || class_names.size() > MAX_CLASSES) throw std::runtime_error("benchmark split exceeds the compiled class limit");
  execution = request.execution;
  if (!request.worker_cpus.empty()) output_cpu = request.worker_cpus.front();
  if (execution) {
   output_execution = &execution->execution();
   files = &execution->files();
   writer_handles = output_execution->reserve(CompileResources::handles(1));
  }
  if (resolution == 0 || resolution > MAX_IMAGE_EXTENT) throw std::runtime_error("benchmark pixel membership is incomplete");
  layout = compute_file_layout(common_math::checked_cast<std::uint32_t>(images.size(), "benchmark image count overflow"), stride);
  (void)common_io::ensure_parent_directory(request.output_path);
  detail::StorageReservationPool storage(request.output_path, {}, execution ? &execution->storage() : nullptr);
  staging = detail::StagedArtifact::create(storage, request.output_path, 0, "benchmark sparse staging");
  // The resident index is written only at seal. Keep the staging EOF aligned
  // so growth cannot allocate a partial EOF page in the next pixel block.
  slots_storage = std::make_unique<CompiledSlotStorage>(staging, layout);
  entries.resize(images.size());
  input_roots.resize(images.size());
 }
 // The enclosing attempt drains a borrowed pipeline before either owner can
 // retire. Only the locally owned service is safe to join from this destructor.
 ~Impl() {
  if (standalone_files) standalone_files->join();
 }
 detail::FileWorkQueue& output() {
  if (execution) return *files;
  // Pixel-only standalone sessions retain their existing useful-worker bound.
  // Initialize the reusable output owner only when a controller writes labels,
  // settles provenance, or seals this writer.
  std::call_once(standalone_output, [&] {
   const auto label_bytes = detail::CompiledLabelWorkspace::required_bytes(0);
   detail::CompileExecutionLimits limits;
   const std::array<detail::CompileWorkspaceProfile, 1> profiles{{{label_bytes, 0}}};
   limits.workspace_plan = std::make_shared<detail::CompileWorkspacePlan>(detail::CompileWorkspacePlan::prepare(
    1, profiles, 0, COMPILED_COMPACTION_SCRATCH_BYTES, common_math::checked_add(label_bytes, COMPILED_COMPACTION_SCRATCH_BYTES, "standalone output workspace overflow")));
   standalone_execution = std::make_unique<detail::CompileExecution>(1, output_cpu ? std::span<const int>(&*output_cpu, 1) : std::span<const int>{}, limits, cancellation);
   standalone_files = std::make_unique<detail::FileWorkQueue>(*standalone_execution, 2);
   output_execution = standalone_execution.get();
   files = standalone_files.get();
  });
  return *files;
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
 if (state.slots_storage->reserve_slot() != slot) throw std::logic_error("benchmark storage slot identity differs");
 state.images.push_back(image);
 state.complete.push_back(0);
 state.header_known.push_back(0);
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
void BenchmarkSplitWriter::settle_labels(std::span<const LabelSettlement> settlements) {
 if (std::ranges::none_of(settlements, [](const auto& value) { return value.ordinal_base || value.ordinal_incomplete; })) return;
 const auto transform = [this, settlements] {
  for (const auto& settlement : settlements) {
   if (!settlement.ordinal_base && !settlement.ordinal_incomplete) continue;
   if (impl_->cancellation.requested()) throw std::runtime_error("dataset compilation cancelled");
   auto labels = impl_->slots_storage->labels(settlement.slot);
   const auto selected = entry(settlement.slot);
   labels.settle_ordinals(selected.num_instances, settlement.ordinal_base, settlement.ordinal_incomplete);
  }
 };
 impl_->output().submit({.work = transform}).wait();
}
void BenchmarkSplitWriter::prepare_pixels(std::uint32_t slot) {
 if (impl_->extent_preparing) {
  const auto [root, id] = identity(slot);
  impl_->extent_preparing(root, id);
 }
 impl_->slots_storage->prepare_pixels(slot);
}
void BenchmarkSplitWriter::write_labels(std::uint32_t slot, ImageEntry entry, ImageAnnotations annotations, std::uint64_t ordinal_base) {
 set_annotation_extent(entry, annotations.labels.size(), annotations.runs.size(), std::to_string(entry.source_image_id));
 // Direct controller callers also hand physical I/O an owned pinned payload.
 auto& files = impl_->output();
 detail::CompiledLabelWorkspace workspace;
 workspace.prepare(0, impl_->output_execution->reserve(
                       {.bytes = detail::CompiledLabelWorkspace::required_bytes(0), .supplied_workspace = true, .completion_workspace = true, .workspace_class = detail::CompileWorkClass::Output}));
 workspace.labels.resize(annotations.labels.size());
 workspace.runs.resize(annotations.runs.size());
 std::ranges::copy(annotations.labels, workspace.labels.begin());
 std::ranges::copy(annotations.runs, workspace.runs.begin());
 auto output = std::make_shared<detail::CompiledLabelWorkspace::OutputLease>(workspace.release_output());
 const auto write = [this, slot, entry, output, ordinal_base] { write_label_output(slot, entry, std::move(*output), ordinal_base); };
 files.submit({.work = write}).wait();
}
void BenchmarkSplitWriter::write_label_output(std::uint32_t slot, ImageEntry entry, detail::CompiledLabelWorkspace::OutputLease output, std::uint64_t ordinal_base) {
 auto& state = *impl_;
 throw_if_benchmark_cancelled(state.cancellation);
 entry.status = ImageStatus::Eligible;
 set_annotation_extent(entry, output.labels.size(), output.runs.size(), std::to_string(entry.source_image_id));
 FileHeader checks{};
 checks.num_classes = common_math::checked_cast<std::uint32_t>(state.class_names.size(), "benchmark class count overflow");
 checks.max_instances_per_image = UINT16_MAX;
 checks.image_width = checks.image_height = state.resolution;
 (void)admit_compiled_annotations(entry, {output.labels, output.runs}, checks, slot, state.cancellation);
 {
  const std::lock_guard lock(state.facts_mutex);
  if (state.images.at(slot).source_image_id != entry.source_image_id) throw std::runtime_error("benchmark label identity or canvas differs from pixels");
 }
 auto labels = state.slots_storage->labels(slot);
 labels.write(entry, std::move(output), ordinal_base);
 {
  const std::lock_guard lock(state.facts_mutex);
  state.entries[slot] = entry;
 }
}
std::shared_ptr<BenchmarkPixelInput> BenchmarkSplitWriter::prepare_pixel(std::size_t slot, std::size_t lane, BenchmarkSourcePublication publication, CompileAllowance allowance,
 std::shared_ptr<const BenchmarkEncodedImage> payload, std::shared_ptr<const detail::CompilePixelWorkspacePlan> plan) {
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
 BenchmarkImageDecoder header_decoder;
 (void)lane;
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
      auto granted = state.execution->execution().try_reserve(CompileResources::handles(1), allowance);
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
    return header_decoder.read_header(encoded, state.actual_dimensions ? 0 : image.source_width, state.actual_dimensions ? 0 : image.source_height);
   };
   auto opened = BenchmarkEncodedImage::open(std::move(file), image.source_image_id, header, state.cancellation, nullptr, allowance, input->payload, std::numeric_limits<std::uint32_t>::max());
   if (input->payload && opened && !input->payload->same_file(*opened)) throw BenchmarkImageError("cached benchmark image changed its admitted inode");
   input->payload = std::move(opened);
   if (!input->payload) throw BenchmarkImageError("cached benchmark image is missing or has an invalid size");
  }
  const auto& header = input->payload->header();
  if (!state.actual_dimensions && ((image.source_width && image.source_width != header.width) || (image.source_height && image.source_height != header.height)))
   throw BenchmarkImageError("benchmark image dimensions do not match annotations");
  if (state.image_opened) state.image_opened(source_root, image.source_image_id);
  if (!plan) plan = prepare_pixel_plan(slot, header);
  if (plan->header() != header) throw BenchmarkImageError("benchmark pixel input changed its prepared header");
  if (plan->policy() != detail::CompilePixelPolicy{state.resolution, state.resolution, state.resize_mode, state.perceptual})
   throw std::logic_error("benchmark pixel preparation changed its output policy");
  input->workspace = std::move(plan);
  throw_if_benchmark_cancelled(state.cancellation);
  {
   const std::lock_guard lock(state.facts_mutex);
   state.images[slot].source_width = header.width;
   state.images[slot].source_height = header.height;
   state.header_known[slot] = 1;
  }
  // The mapping owns its inode after the local image file closes. The cached
  // directory owns its separate child grant until directory replacement or writer teardown.
  allowance.retire_descriptors();
 } catch (const std::exception& error) {
  if (!dynamic_cast<const BenchmarkImageError*>(&error) && !dynamic_cast<const InvalidImageError*>(&error)) throw;
  throw_if_benchmark_cancelled(state.cancellation);
  throw BenchmarkImageReadError(image.source_index, image.source_image_id, error.what());
 }
 return input;
}
CompileAllowance BenchmarkSplitWriter::pixel_input_allowance(const BenchmarkPixelInput& input) const { return input.payload->allowance(); }
std::size_t BenchmarkSplitWriter::pixel_input_bytes(const BenchmarkPixelInput& input) const { return input.payload->size(); }
CompileResources BenchmarkSplitWriter::pixel_resources(const BenchmarkPixelInput& input) const { return input.workspace->resources(); }
CompileResources BenchmarkSplitWriter::preload_resources(const BenchmarkPixelInput& input) const {
 auto resources = pixel_resources(input);
 // Keep the following decoder/resizer arena naturally aligned.
 const auto encoded = common_math::checked_add(pixel_input_bytes(input), std::size_t{63}, "benchmark preload alignment overflow") / 64 * 64;
 resources.bytes = common_math::checked_add(resources.bytes, static_cast<std::uint64_t>(encoded), "benchmark preload workspace overflow");
 return resources;
}
void BenchmarkSplitWriter::preload_pixel(BenchmarkPixelInput& input, CompileAllowance& allowance) {
 const auto bytes = preload_resources(input).bytes - pixel_resources(input).bytes;
 auto encoded = allowance.split_storage(bytes);
 input.payload = input.payload->preload(std::move(encoded), impl_->cancellation);
}
std::shared_ptr<const detail::CompilePixelWorkspacePlan> BenchmarkSplitWriter::prepare_pixel_plan(std::size_t slot, const BenchmarkImageHeader& header) const {
 detail::CompilePixelPolicy policy;
 std::filesystem::path root;
 std::uint64_t id = 0;
 {
  const std::lock_guard lock(impl_->facts_mutex);
  policy = {impl_->resolution, impl_->resolution, impl_->resize_mode, impl_->perceptual};
  if (impl_->pixel_preparing) {
   const auto& image = impl_->images.at(slot);
   id = image.source_image_id;
   root = impl_->input_roots.at(slot).empty() ? impl_->sources.at(image.source_index).root : impl_->input_roots.at(slot);
  }
 }
 throw_if_benchmark_cancelled(impl_->cancellation);
 if (impl_->pixel_preparing) impl_->pixel_preparing(root, id);
 auto plan = std::make_shared<const detail::CompilePixelWorkspacePlan>(detail::compile_pixel_workspace_plan(header, policy));
 throw_if_benchmark_cancelled(impl_->cancellation);
 return plan;
}
void BenchmarkSplitWriter::write_pixel(std::size_t slot, std::size_t lane) {
 auto input = prepare_pixel(slot, lane);
 if (!input) return;
 detail::CompileExecutionLimits limits;
 limits.workspace_plan = std::make_shared<detail::CompileWorkspacePlan>(detail::compile_workspace_plan(1, impl_->resolution, impl_->perceptual, 0, 0, BenchmarkEncodedImage::normal_window_bytes));
 detail::CompileExecution execution(1, {}, limits, impl_->cancellation);
 detail::CompilePixelWorkspace workspace;
 detail::FileWorkQueue files(execution, 1);
 files.submit({.work = [&] { prepare_pixels(common_math::checked_cast<std::uint32_t>(slot, "benchmark pixel slot overflow")); }}).wait();
 execution.run(CompileTaskKind::Pixels, pixel_resources(*input), [&](std::size_t) { write_pixel(slot, lane, input, execution.current_allowance(), workspace); });
}
void BenchmarkSplitWriter::write_pixel(std::size_t slot, std::size_t lane, const std::shared_ptr<BenchmarkPixelInput>& input, CompileAllowance allowance, detail::CompilePixelWorkspace& scratch) {
 if (!input || image_complete(slot)) return;
 auto& state = *impl_;
 (void)lane;
 const auto& header = input->payload->header();
 const auto& plan = *input->workspace;
 if (plan.policy() != detail::CompilePixelPolicy{state.resolution, state.resolution, state.resize_mode, state.perceptual})
  throw std::logic_error("benchmark pixel output policy changed after preparation");
 auto memory = allowance.workspace();
 if (memory.size() < plan.bytes()) throw std::logic_error("benchmark pixels have no admitted arena");
 throw_if_benchmark_cancelled(state.cancellation);
 try {
  auto destination = state.slots_storage->pixels(common_math::checked_cast<std::uint32_t>(slot, "benchmark pixel slot overflow"));
  detail::CompilePixelObserver observer;
  if (state.trace.accepts("compile.pixel"))
   observer = [&](const detail::CompilePixelObservation& value) {
    trace_benchmark_event(state.trace, "compile.pixel", [&] {
     auto fields = benchmark_observation_fields(value);
     const std::lock_guard lock(state.facts_mutex);
     const auto& image = state.images.at(slot);
     fields["source"] = image.source_index;
     fields["image_id"] = image.source_image_id;
     fields["slot"] = slot;
     return fields;
    });
   };
  scratch.write(plan, input->payload->encoded(), header, memory, {destination.data(), plan.destination()}, +[] {}, observer);
 } catch (const detail::CompileImageError& error) {
  throw_if_benchmark_cancelled(state.cancellation);
  const std::lock_guard lock(state.facts_mutex);
  const auto& image = state.images.at(slot);
  throw BenchmarkImageReadError(image.source_index, image.source_image_id, error.what());
 }
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
  if (impl_->directory.get() >= 0 && impl_->directory_root == root) {
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
 const auto cpus = request.worker_cpus.empty() ? mmltk::common::system::allowed_cpu_set() : std::vector<int>(request.worker_cpus.begin(), request.worker_cpus.end());
 const int workers = std::max(
  1, std::min({request.num_workers, common_math::checked_cast<int>(cpus.size(), "benchmark CPU count overflow"), common_math::checked_cast<int>(slots.size(), "benchmark slot count overflow")}));
 detail::CompileExecutionLimits limits;
 limits.workspace_plan =
  std::make_shared<detail::CompileWorkspacePlan>(detail::compile_workspace_plan(workers, impl_->resolution, impl_->perceptual, 0, 0, BenchmarkEncodedImage::normal_window_bytes));
 detail::CompileExecution execution(workers, cpus, limits, request.cancel_requested);
 detail::FileWorkQueue files(execution, execution.workers());
 std::vector<std::unique_ptr<detail::CompilePixelWorkspace>> workspaces;
 for (std::size_t i = 0; i < execution.workers(); ++i) workspaces.push_back(std::make_unique<detail::CompilePixelWorkspace>());
 std::vector<detail::CompileExecution::Completion> active;
 active.reserve(execution.workers());
 try {
  for (const auto slot : slots) {
   // The existing strict standalone writer admits each immutable header before
   // its actual workspace request; submitted pixels may finish independently.
   auto input = prepare_pixel(slot, 0);
   if (!input) continue;
   files.submit({.work = [&] { prepare_pixels(common_math::checked_cast<std::uint32_t>(slot, "benchmark pixel slot overflow")); }}).wait();
   active.push_back(
    execution.submit({.kind = CompileTaskKind::Pixels, .resources = pixel_resources(*input), .work = [this, slot, input, &workspaces](std::size_t lane, const CompileAllowance& allowance) {
    write_pixel(slot, lane, input, allowance, *workspaces[lane]);
   }, .retain_workspace = false}));
   if (active.size() == execution.workers()) {
    for (const auto& work : active) work.wait();
    active.clear();
   }
  }
  for (const auto& work : active) work.wait();
 } catch (...) {
  execution.cancel();
  try {
   execution.join();
  } catch (...) {}
  throw;
 }
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
   write_label_output(physical_slot, expected, {});
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
 state.slots_storage->quiesce();
 // Claimed but never prepared tail slots remain inactive sparse capacity.
 state.staging.resize_sparse(state.layout.index_offset);
 {
  const std::lock_guard lock(state.directory_mutex);
  state.directory = {};
  state.directory_source = std::numeric_limits<std::uint16_t>::max();
 }
 trace_benchmark_event(request.trace, "benchmark.compaction.started", [&] { return nlohmann::json{{"split", request.split.name}, {"images", slots.size()}, {"bytes", state.layout.total_size}}; });
 BenchmarkCostSpan compaction_cost(
  request.trace, BenchmarkCostKind::Compaction, [&] { return nlohmann::json{{"split", request.split.name}, {"rows_revisited", index.size()}, {"bytes", state.layout.total_size}}; });
 state.layout = compact_compiled_file(state.staging.file(), state.layout, index, request.cancel_requested, request.compaction,
  CompiledStorageMutation::for_artifact(state.staging), request.compaction_scratch,
  request.trace.accepts("benchmark.compaction.summary") ? CompiledCompactionObserver{[&](const auto& value) {
   trace_benchmark_event(request.trace, "benchmark.compaction.summary", [&] {
    auto fields = benchmark_observation_fields(value);
    fields["split"] = request.split.name;
    return fields;
   });
  }} : CompiledCompactionObserver{});
 compaction_cost.finish();
 trace_benchmark_event(request.trace, "benchmark.compaction.completed", [&] { return nlohmann::json{{"split", request.split.name}, {"images", slots.size()}, {"bytes", state.layout.total_size}}; });
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
 trace_benchmark_event(request.trace, "benchmark.split_sync.started", [&] { return nlohmann::json{{"split", request.split.name}, {"bytes", state.layout.total_size}}; });
 detail::CompileObservationClock sync_clock{request.trace.accepts("benchmark.split_sync.completed")};
 output.sync_data();
 trace_benchmark_event(request.trace, "benchmark.split_sync.completed", [&] { return nlohmann::json{{"split", request.split.name}, {"bytes", state.layout.total_size}, {"duration_ns", sync_clock.elapsed()}}; });
 const auto persisted = read_compiled_header(output);
 (void)validate_compiled_file_sections(persisted, output.size());
 if (std::memcmp(&persisted, &header, sizeof(header))) throw std::runtime_error("staged benchmark header differs");
 state.staging.close();
 throw_if_benchmark_cancelled(request.cancel_requested);
 CompiledDatasetInfo info{request.output_path, header.num_images, header.image_width, header.image_height, header.channels, header.max_instances_per_image, std::move(class_catalog)};
 return {std::move(state.staging), std::move(info)};
}
void BenchmarkSplitWriter::finish(const BenchmarkWriteRequest& request) {
 auto& files = impl_->output();
 auto scratch = impl_->output_execution->reserve(
  {.bytes = COMPILED_COMPACTION_SCRATCH_BYTES,
   .supplied_workspace = true,
   .completion_workspace = true,
   .workspace_class = detail::CompileWorkClass::Output,
   .workspace_window = detail::CompileWorkspaceWindow::Compaction});
 std::optional<BenchmarkSealedSplit> sealed;
 auto seal_request = request;
 seal_request.compaction_scratch = scratch.workspace();
 files.submit({.work = [&] { sealed = seal(seal_request); }}).wait();
 scratch = {};
 sealed->artifact.publish(request.output_path, request.cancel_requested, detail::StagedArtifact::Publication::DurableReplace, request.overwrite);
}
void write_benchmark_split(const BenchmarkWriteRequest& request) {
 for (const auto& image : request.split.images)
  if (!image.source_width || !image.source_height || image.source_index >= request.split.sources.size()) throw std::runtime_error("benchmark image metadata is incomplete");
 BenchmarkSplitWriter writer(request);
 writer.write_remaining(request);
 writer.finish(request);
}
std::array<BenchmarkSealedSplit, 2> seal_benchmark_splits(BenchmarkCompilePipeline& pipeline, const std::function<BenchmarkSealedSplit(std::size_t, std::span<std::byte>)>& seal) {
 std::array<BenchmarkSealedSplit, 2> results{};
 std::array<detail::CompileExecution::Completion, 2> completions;
 std::exception_ptr failure;
 for (std::size_t i = 0; i < results.size(); ++i) {
  try {
   auto scratch = pipeline.execution().reserve(
    {.bytes = COMPILED_COMPACTION_SCRATCH_BYTES,
     .supplied_workspace = true,
     .completion_workspace = true,
     .workspace_class = detail::CompileWorkClass::Output,
     .workspace_window = detail::CompileWorkspaceWindow::Compaction});
   completions[i] = pipeline.files().submit({.work = [&, i, scratch] { results[i] = seal(i, scratch.workspace()); }});
  } catch (...) {
   if (!failure) failure = std::current_exception();
   break;
  }
 }
 for (const auto& completion : completions)
  if (completion) {
   try {
    completion.wait();
   } catch (...) {
    if (!failure) failure = std::current_exception();
   }
  }
 if (failure) std::rethrow_exception(failure);
 return results;
}
}  // namespace mmltk::backend::data::benchmark_internal
