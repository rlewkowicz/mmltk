#include "src/backend/data/compiler/dataset_compiler.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "src/backend/data/compiled_file.h"
#include "src/common/io/file_memory.h"
#include "src/common/system/cpu_affinity.h"
// CLEANUP-IGNORE: This dataset compiler unit imports the exact storage and system owners it consumes.
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
#include "src/backend/data/compiler/detail/dataset_compiler_internal.h"
#include "src/backend/data/detail/staged_artifact.h"
#include "src/backend/data/detail/compiled_slot_storage.h"
#include "src/backend/data/detail/file_work_queue.h"
#include "src/backend/data/detail/compile_workspace.h"
namespace mmltk::backend::data {
using mmltk::common::io::FileHandle;
using mmltk::common::io::publish_staged_path_atomically;
using mmltk::common::system::allowed_cpu_set;
using DeliveryMode = detail::CompileProgressDeliveryMode;
ProgressEstimate estimate_progress(const std::uint64_t completed, const std::uint64_t total, const std::uint64_t elapsed_seconds) noexcept {
 if (elapsed_seconds == 0U || completed == 0U) { return {}; }
 const std::uint64_t throughput = completed / elapsed_seconds;
 if (completed >= total) { return {.remaining_seconds = 0U, .throughput_per_second = throughput}; }
 const __uint128_t remaining = static_cast<__uint128_t>(total - completed) * elapsed_seconds / completed;
 return {
  .remaining_seconds = remaining > std::numeric_limits<std::uint64_t>::max() ? std::numeric_limits<std::uint64_t>::max() : static_cast<std::uint64_t>(remaining), .throughput_per_second = throughput
 };
}
CompileTelemetry::CompileTelemetry() noexcept : started_at_rep_(now_().time_since_epoch().count()) {}
CompileTelemetry::CompileTelemetry(size_t num_images, CompileProgressObserver observer, Now now)
    : num_images_(num_images), observer_(observer), now_(now), started_at_rep_(now_().time_since_epoch().count()) {
 if (!observer_.report) return;
 using Delivery = detail::CompileProgressDelivery<CompileProgress>;
 const bool diagnostic = mmltk::common::logging::enabled(spdlog::level::trace);
 delivery_ = std::make_unique<Delivery>(publication_mutex_, [this] { return snapshot(); }, [this](const CompileProgress& value) { observer_(value); }, Delivery::Failure{},
  diagnostic ? Delivery::Diagnostic{[](const Delivery::Counts& counts) {
  mmltk::common::logging::trace([&](spdlog::logger& log) {
   log.trace("compiler.progress.delivery produced={} materialized={} coalesced={} delivered={} pending={} high_water={}", counts.produced, counts.materialized, counts.coalesced, counts.delivered,
    counts.pending, counts.high_water);
  });
 }}
             : Delivery::Diagnostic{});
}
CompileTelemetry::~CompileTelemetry() { delivery_.reset(); }
void CompileTelemetry::flush() {
 if (delivery_) delivery_->flush();
}
CompileProgress CompileTelemetry::snapshot() const noexcept { return snapshot(phase_.load(std::memory_order_relaxed)); }
CompileProgress CompileTelemetry::snapshot(DatasetCompilePhase phase) const noexcept {
 const size_t num_images = num_images_.load(std::memory_order_relaxed);
 const size_t label_done = std::min(labels_.done.load(std::memory_order_relaxed), num_images);
 const size_t pixel_done = std::min(pixels_.done.load(std::memory_order_relaxed), num_images);
 const size_t label_active = labels_.active_workers.load(std::memory_order_relaxed);
 const size_t pixel_active = pixels_.active_workers.load(std::memory_order_relaxed);
 if (phase == DatasetCompilePhase::Labels || phase == DatasetCompilePhase::Pixels) phase = label_done < num_images ? DatasetCompilePhase::Labels : DatasetCompilePhase::Pixels;
 const size_t total = num_images * 2U + 1U;
 const Clock::duration::rep started_at_rep = started_at_rep_.load(std::memory_order_relaxed);
 const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now_().time_since_epoch() - Clock::duration{started_at_rep}).count();
 const std::uint64_t elapsed_seconds = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U;
 const auto progress = [&](const size_t done) {
  const ProgressEstimate estimate = estimate_progress(done, total, elapsed_seconds);
  return CompileProgress{
   .done = done,
   .total = total,
   .elapsed_seconds = elapsed_seconds,
   .remaining_seconds = estimate.remaining_seconds,
   .throughput_per_second = estimate.throughput_per_second,
   .phase = phase,
   .tracks =
    {.acquisition = {.total_known = true, .complete = true, .activity = DatasetCompileActivity::Unnecessary},
     .labels =
      {.completed = label_done,
       .total = num_images,
       .total_known = true,
       .active = phase != DatasetCompilePhase::Planning && label_done != num_images,
       .complete = label_done == num_images,
       .activity = label_done == num_images ? DatasetCompileActivity::Complete : DatasetCompileActivity::Preparing},
     .pixels =
      {.completed = pixel_done,
       .total = num_images,
       .total_known = true,
       .active = (pixel_active != 0 || phase == DatasetCompilePhase::Pixels) && pixel_done != num_images,
       .complete = pixel_done == num_images,
       .activity = pixel_done == num_images ? DatasetCompileActivity::Complete : DatasetCompileActivity::Compiling}},
   .active_workers = label_active + pixel_active,
   .dropped_instances = dropped_instances_.load(std::memory_order_relaxed)
  };
 };
 if (phase == DatasetCompilePhase::Planning) { return progress(0U); }
 if (phase == DatasetCompilePhase::Compacting) {
  auto value = progress(label_done + pixel_done);
  value.compacted_bytes = compacted_bytes_.load(std::memory_order_relaxed);
  value.compaction_bytes = compaction_bytes_.load(std::memory_order_relaxed);
  value.active_workers = 1;
  return value;
 }
 if (phase == DatasetCompilePhase::Syncing || phase == DatasetCompilePhase::Publishing) {
  CompileProgress complete = progress(total);
  complete.active_workers = 0U;
  return complete;
 }
 return progress(label_done + pixel_done);
}
void CompileTelemetry::reset(const size_t num_images) noexcept {
 labels_.done.store(0U, std::memory_order_relaxed);
 labels_.active_workers.store(0U, std::memory_order_relaxed);
 pixels_.done.store(0U, std::memory_order_relaxed);
 pixels_.active_workers.store(0U, std::memory_order_relaxed);
 compacted_bytes_.store(0U, std::memory_order_relaxed);
 compaction_bytes_.store(0U, std::memory_order_relaxed);
 dropped_instances_.store(0U, std::memory_order_relaxed);
 num_images_.store(num_images, std::memory_order_relaxed);
 const Clock::duration::rep started_at_rep = now_().time_since_epoch().count();
 started_at_rep_.store(started_at_rep, std::memory_order_relaxed);
 publish_semantic(DatasetCompilePhase::Planning);
}
void CompileTelemetry::add_label_done(const size_t count) noexcept {
 const auto done = labels_.done.fetch_add(count, std::memory_order_relaxed) + count;
 publish_counter(done == num_images_.load(std::memory_order_relaxed));
}
void CompileTelemetry::add_pixel_done(const size_t count) noexcept {
 const auto done = pixels_.done.fetch_add(count, std::memory_order_relaxed) + count;
 publish_counter(done == num_images_.load(std::memory_order_relaxed));
}
void CompileTelemetry::enter_labels() noexcept { publish_semantic(DatasetCompilePhase::Labels); }
void CompileTelemetry::enter_pixels() noexcept { publish_semantic(DatasetCompilePhase::Pixels); }
void CompileTelemetry::begin_label_worker() noexcept { labels_.active_workers.fetch_add(1U, std::memory_order_relaxed); }
void CompileTelemetry::end_label_worker() noexcept { labels_.active_workers.fetch_sub(1U, std::memory_order_relaxed); }
void CompileTelemetry::begin_pixel_worker() noexcept { pixels_.active_workers.fetch_add(1U, std::memory_order_relaxed); }
void CompileTelemetry::end_pixel_worker() noexcept { pixels_.active_workers.fetch_sub(1U, std::memory_order_relaxed); }
void CompileTelemetry::enter_syncing() noexcept { publish_semantic(DatasetCompilePhase::Syncing); }
void CompileTelemetry::compacting(std::size_t done, std::size_t total) noexcept {
 const std::lock_guard lock(publication_mutex_);
 compacted_bytes_.store(done, std::memory_order_relaxed);
 compaction_bytes_.store(total, std::memory_order_relaxed);
 phase_.store(DatasetCompilePhase::Compacting, std::memory_order_relaxed);
 if (delivery_) delivery_->changed(done == 0 || done == total ? DeliveryMode::Boundary : DeliveryMode::Dirty);
}
void CompileTelemetry::enter_publishing() noexcept { publish_semantic(DatasetCompilePhase::Publishing); }
void CompileTelemetry::set_dropped_instances(const std::uint64_t count) noexcept {
 dropped_instances_.store(count, std::memory_order_relaxed);
 publish_counter();
}
void CompileTelemetry::publish_semantic(const DatasetCompilePhase phase) noexcept {
 const std::lock_guard lock(publication_mutex_);
 phase_.store(phase, std::memory_order_relaxed);
 if (delivery_) delivery_->changed(DeliveryMode::Lifecycle);
}
void CompileTelemetry::publish_counter(bool boundary) noexcept {
 if (!delivery_) return;
 const std::lock_guard lock(publication_mutex_);
 delivery_->changed(boundary ? DeliveryMode::Boundary : DeliveryMode::Dirty);
}
void compiler_internal::ProgressCounter::begin_worker() noexcept { update_worker_count(true); }
void compiler_internal::ProgressCounter::update_worker_count(const bool starting) noexcept {
 if (telemetry == nullptr) { return; }
 if (stage == Stage::kLabels) {
  starting ? telemetry->begin_label_worker() : telemetry->end_label_worker();
 } else {
  starting ? telemetry->begin_pixel_worker() : telemetry->end_pixel_worker();
 }
}
void compiler_internal::ProgressCounter::add_completed(const size_t count) noexcept {
 if (telemetry == nullptr) { return; }
 if (stage == Stage::kLabels) {
  telemetry->add_label_done(count);
 } else {
  telemetry->add_pixel_done(count);
 }
}
void compiler_internal::ProgressCounter::end_worker() noexcept { update_worker_count(false); }
DatasetCompilePlan DatasetCompiler::prepare(CompilerConfig config, const std::vector<std::string>& splits, const mmltk::common::concurrency::CancellationObservation cancellation) {
 mmltk::common::logging::ScopedProfile profile{"compiler.prepare"};
 if (splits.empty()) { throw std::runtime_error("dataset compile plan requires at least one split"); }
 if (const auto valid = validate_compiler_config(config); !valid) { throw std::runtime_error(std::string(compiler_config_validation_message(valid.error()))); }
 if (config.worker_cpus.empty()) { config.worker_cpus = allowed_cpu_set(); }
 if (config.worker_cpus.empty()) { throw std::runtime_error("dataset compilation requires at least one allowed CPU"); }
 compiler_internal::DatasetScan scan = compiler_internal::scan_dataset(config, splits, cancellation);
 DatasetCompilePlan plan;
 plan.config = std::move(config);
 plan.class_catalog = std::move(scan.class_catalog);
 plan.source_category_base = scan.source_category_base;
 plan.splits = std::move(scan.splits);
 return plan;
}
void DatasetCompiler::compile(const DatasetCompilePlan& plan, const size_t split_index, CompileTelemetry* const telemetry, const mmltk::common::concurrency::CancellationObservation cancellation) {
 mmltk::common::logging::ScopedProfile profile{"compiler.total"};
 if (split_index >= plan.splits.size()) { throw std::runtime_error("dataset compile split index is out of range"); }
 CompilerConfig effective_config = plan.config;
 effective_config.split = plan.splits[split_index].split;
 if (const auto valid = validate_compiler_config(effective_config); !valid) { throw std::runtime_error(std::string(compiler_config_validation_message(valid.error()))); }
 const auto throw_if_cancelled = [&] {
  if (cancellation.requested()) { throw std::runtime_error("dataset compilation cancelled"); }
 };
 throw_if_cancelled();
 const std::filesystem::path split_dir = std::filesystem::path(effective_config.source_dir) / effective_config.split;
 const std::filesystem::path out_dir = effective_config.output_dir;
 std::filesystem::create_directories(out_dir);
 const uint32_t width = effective_config.target_width;
 const uint32_t height = effective_config.target_height;
 const uint32_t channels = 3;
 const size_t image_stride = static_cast<size_t>(channels) * height * width * sizeof(float);
 const int configured_workers = compiler_internal::resolve_num_workers(effective_config.num_workers, std::span<const int>(effective_config.worker_cpus));
 const auto worker_cpus = std::span<const int>(effective_config.worker_cpus).first(static_cast<std::size_t>(configured_workers));
 mmltk::common::logging::profile_set_value("compiler.num_workers", static_cast<std::size_t>(configured_workers));
 const auto& class_catalog = plan.class_catalog;
 if (class_catalog.empty() || plan.source_category_base > 1U) throw std::runtime_error("invalid generic compile catalog");
 const uint32_t num_images = plan.splits[split_index].image_count;
 mmltk::common::logging::profile_set_value("compiler.num_images", num_images);
 mmltk::common::logging::profile_set_value("compiler.image_stride_bytes", image_stride);
 if (telemetry != nullptr) {
  telemetry->reset(num_images);
  telemetry->enter_labels();
  telemetry->enter_pixels();
 }
 compiler_internal::ProgressCounter label_progress{telemetry, compiler_internal::ProgressCounter::Stage::kLabels};
 compiler_internal::ProgressCounter pixel_progress{telemetry, compiler_internal::ProgressCounter::Stage::kPixels};
 mmltk::common::logging::debug([&](spdlog::logger& log) {
  log.debug("[compile] {}: {} images, {} shared workers, target {}x{}, stride={} bytes/img", effective_config.split, num_images, configured_workers, width, height, image_stride);
 });
 FileLayout layout;
 {
  mmltk::common::logging::ScopedProfile layout_profile{"compiler.compute_file_layout"};
  layout = compute_file_layout(num_images, image_stride);
 }
 mmltk::common::logging::debug([&](spdlog::logger& log) {
  log.debug("[compile] Pixel layout: pixel_offset={} ({:.1f} MB aligned), pixels={:.2f} GB", layout.data_offset, static_cast<double>(layout.data_offset) / (1024.0 * 1024.0),
   static_cast<double>((static_cast<std::size_t>(num_images) * image_stride)) / (1024.0 * 1024.0 * 1024.0));
 });
 const std::filesystem::path out_path = out_dir / (effective_config.split + ".bin");
 detail::StorageReservationPool destination(out_path);
 auto staged = detail::StagedArtifact::create(destination, out_path, 0, "directory compiled staging");
 auto& fd = staged.file();
 detail::CompiledSlotStorage storage(staged, layout);
 constexpr std::array classes{detail::CompileWorkClass::Extraction, detail::CompileWorkClass::Annotations, detail::CompileWorkClass::Pixels, detail::CompileWorkClass::Output};
 const auto label_window = compiler_internal::DirectoryLabels::resources(2048, compiler_internal::directory_line_window).bytes;
 detail::CompileExecutionLimits limits;
 limits.workspace_plan = std::make_shared<detail::CompileWorkspacePlan>(detail::compile_workspace_plan(static_cast<std::size_t>(configured_workers),
  {width, height, effective_config.resize_mode, effective_config.perceptual_downscale}, classes, {label_window, label_window}, 0, compiler_internal::directory_encoded_window));
 detail::CompileExecution::Observer observer;
 if (effective_config.observation && effective_config.observation->execution)
  observer = [&](std::string_view event, const detail::CompileExecutionSnapshot& snapshot) { effective_config.observation->execution(effective_config.observation->context, event, snapshot); };
 detail::CompileExecution execution(
  static_cast<std::size_t>(configured_workers), worker_cpus, limits, cancellation, std::move(observer), std::make_exception_ptr(std::runtime_error("dataset compilation cancelled")));
 auto output = execution.open_class(detail::CompileWorkClass::Output);
 detail::FileWorkQueue files(execution, std::max<std::size_t>(2, static_cast<std::size_t>(configured_workers) * 2), std::min<std::size_t>(2, execution.workers()));
 auto label_blocks = compiler_internal::compile_directory_images(split_dir, num_images, effective_config, class_catalog, plan.source_category_base, storage, execution, files,
  telemetry ? &label_progress : nullptr, telemetry ? &pixel_progress : nullptr, cancellation);
 if (telemetry) telemetry->set_dropped_instances(label_blocks.dropped_instances);
 throw_if_cancelled();
 files.join();
 storage.quiesce();
 auto compaction = execution.reserve(
  {.bytes = COMPILED_COMPACTION_SCRATCH_BYTES,
   .supplied_workspace = true,
   .completion_workspace = true,
   .workspace_class = detail::CompileWorkClass::Output,
   .workspace_window = detail::CompileWorkspaceWindow::Compaction});
 files
  .submit(
   {.work =
     [&] {
  layout = compact_compiled_file(fd, layout, label_blocks.index, cancellation,
   telemetry ? CompiledCompactionProgress{[&](std::size_t done, std::size_t total) { telemetry->compacting(done, total); }} : CompiledCompactionProgress{},
   {[&](std::size_t offset, std::size_t bytes) { staged.allocate_range(offset, bytes); }, [&](std::size_t offset, std::size_t bytes) { return staged.reclaim_range(offset, bytes); },
    [&](std::size_t bytes) { staged.resize_sparse(bytes); }},
   compaction.workspace());
  mmltk::common::logging::debug([&](spdlog::logger& log) { log.debug("[compile] Layout: total={:.2f} GB", static_cast<double>(layout.total_size) / (1024.0 * 1024.0 * 1024.0)); });
  staged.allocate_range(0, compiled_align_up(sizeof(FileHeader), PAGE_SIZE));
  staged.allocate_range(layout.index_offset, compiled_align_up(layout.index_size, PAGE_SIZE));
  staged.resize_sparse(layout.total_size);
  const FileHeader header = make_file_header(
   FileHeaderInputs{
    num_images,
    width,
    height,
    channels,
    label_blocks.max_instances_per_image,
    image_stride,
    effective_config.resize_mode,
   },
   class_catalog.names(), layout);
  validate_compiled_header(header);
  throw_if_cancelled();
  compiler_internal::write_metadata_blocks(fd, layout, header, label_blocks, cancellation);
  throw_if_cancelled();
  if (telemetry != nullptr) { telemetry->enter_syncing(); }
  fd.sync_data();
  throw_if_cancelled();
  const FileHeader staged_header = read_compiled_header(fd);
  (void)validate_compiled_file_sections(staged_header, fd.size());
  staged.close();
 }})
  .wait();
 compaction = {};
 throw_if_cancelled();
 if (telemetry != nullptr) {
  telemetry->enter_publishing();
  telemetry->flush();
 }
 throw_if_cancelled();
 staged.publish(out_path, cancellation);
 mmltk::common::logging::debug([&](spdlog::logger& log) { log.debug("[compile] Written {} ({:.2f} GB)", out_path.string(), static_cast<double>(layout.total_size) / (1024.0 * 1024.0 * 1024.0)); });
}
}  // namespace mmltk::backend::data
