#include "src/backend/data/dataset_compiler.h"

#include <spdlog/spdlog.h>

#include <algorithm>
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

#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/compiled_format.h"
#include "src/common/io/file_memory.h"
#include "src/common/system/cpu_affinity.h"

// CLEANUP-IGNORE: This dataset compiler unit imports the exact storage and system owners it consumes.

import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;

#include "detail/dataset_compiler_internal.h"
#include "detail/staging_file_cleanup.h"

namespace mmltk::backend::data {

using mmltk::common::io::FileHandle;
using mmltk::common::io::publish_staged_path_atomically;
using mmltk::common::system::allowed_cpu_set;

ProgressEstimate estimate_progress(const std::uint64_t completed, const std::uint64_t total, const std::uint64_t elapsed_seconds) noexcept {
    if (elapsed_seconds == 0U || completed == 0U) { return {}; }
    const std::uint64_t throughput = completed / elapsed_seconds;
    if (completed >= total) { return {.remaining_seconds = 0U, .throughput_per_second = throughput}; }
    const __uint128_t remaining = static_cast<__uint128_t>(total - completed) * elapsed_seconds / completed;
    return {.remaining_seconds = remaining > std::numeric_limits<std::uint64_t>::max() ? std::numeric_limits<std::uint64_t>::max()
                                                                                       : static_cast<std::uint64_t>(remaining),
            .throughput_per_second = throughput};
}

CompileProgress CompileTelemetry::snapshot() const noexcept { return snapshot(phase_.load(std::memory_order_relaxed)); }

CompileProgress CompileTelemetry::snapshot(const DatasetCompilePhase phase) const noexcept {
    const size_t num_images = num_images_.load(std::memory_order_relaxed);
    const size_t label_done = std::min(labels_.done.load(std::memory_order_relaxed), num_images);
    const size_t pixel_done = std::min(pixels_.done.load(std::memory_order_relaxed), num_images);
    const size_t label_active = labels_.active_workers.load(std::memory_order_relaxed);
    const size_t pixel_active = pixels_.active_workers.load(std::memory_order_relaxed);
    const size_t total = num_images * 2U + 1U;
    const Clock::duration::rep started_at_rep = started_at_rep_.load(std::memory_order_relaxed);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now_().time_since_epoch() - Clock::duration{started_at_rep}).count();
    const std::uint64_t elapsed_seconds = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U;
    const auto progress = [&](const size_t done) {
        const ProgressEstimate estimate = estimate_progress(done, total, elapsed_seconds);
        return CompileProgress{.done = done,
                               .total = total,
                               .elapsed_seconds = elapsed_seconds,
                               .remaining_seconds = estimate.remaining_seconds,
                               .throughput_per_second = estimate.throughput_per_second,
                               .phase = phase,
                               .label_done = label_done,
                               .label_total = num_images,
                               .pixel_done = pixel_done,
                               .pixel_total = num_images,
                               .active_workers = label_active + pixel_active,
                               .dropped_instances = dropped_instances_.load(std::memory_order_relaxed)};
    };
    if (phase == DatasetCompilePhase::Planning) { return progress(0U); }
    if (phase == DatasetCompilePhase::Syncing || phase == DatasetCompilePhase::Publishing) {
        CompileProgress complete = progress(total);
        complete.label_done = num_images;
        complete.pixel_done = num_images;
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
    dropped_instances_.store(0U, std::memory_order_relaxed);
    num_images_.store(num_images, std::memory_order_relaxed);
    published_counter_quantum_.store(0U, std::memory_order_relaxed);
    const Clock::duration::rep started_at_rep = now_().time_since_epoch().count();
    started_at_rep_.store(started_at_rep, std::memory_order_relaxed);
    publish_semantic(DatasetCompilePhase::Planning);
}

void CompileTelemetry::add_label_done(const size_t count) noexcept {
    labels_.done.fetch_add(count, std::memory_order_relaxed);
    publish_counter();
}

void CompileTelemetry::add_pixel_done(const size_t count) noexcept {
    pixels_.done.fetch_add(count, std::memory_order_relaxed);
    publish_counter();
}

void CompileTelemetry::enter_labels() noexcept { publish_semantic(DatasetCompilePhase::Labels); }

void CompileTelemetry::enter_pixels() noexcept { publish_semantic(DatasetCompilePhase::Pixels); }

void CompileTelemetry::begin_label_worker() noexcept { labels_.active_workers.fetch_add(1U, std::memory_order_relaxed); }

void CompileTelemetry::end_label_worker() noexcept { labels_.active_workers.fetch_sub(1U, std::memory_order_relaxed); }

void CompileTelemetry::begin_pixel_worker() noexcept { pixels_.active_workers.fetch_add(1U, std::memory_order_relaxed); }

void CompileTelemetry::end_pixel_worker() noexcept { pixels_.active_workers.fetch_sub(1U, std::memory_order_relaxed); }

void CompileTelemetry::enter_syncing() noexcept { publish_semantic(DatasetCompilePhase::Syncing); }

void CompileTelemetry::enter_publishing() noexcept { publish_semantic(DatasetCompilePhase::Publishing); }

void CompileTelemetry::set_dropped_instances(const std::uint64_t count) noexcept {
    dropped_instances_.store(count, std::memory_order_relaxed);
    publish_counter();
}

void CompileTelemetry::publish_semantic(const DatasetCompilePhase phase) noexcept {
    const std::lock_guard lock(publication_mutex_);
    phase_.store(phase, std::memory_order_relaxed);
    if (observer_.report != nullptr) { observer_(snapshot(phase)); }
}

void CompileTelemetry::publish_counter() noexcept {
    if (observer_.report == nullptr) return;
    constexpr std::size_t kCounterQuantum = 64U;
    const std::size_t completed = labels_.done.load(std::memory_order_relaxed) + pixels_.done.load(std::memory_order_relaxed);
    const std::size_t quantum = completed / kCounterQuantum;
    std::size_t published = published_counter_quantum_.load(std::memory_order_relaxed);
    if (quantum == 0U || quantum <= published ||
        !published_counter_quantum_.compare_exchange_strong(published, quantum, std::memory_order_relaxed))
        return;
    const std::lock_guard lock(publication_mutex_);
    observer_(snapshot());
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

DatasetCompilePlan DatasetCompiler::prepare(CompilerConfig config, const std::vector<std::string>& splits,
                                            const mmltk::common::concurrency::CancellationObservation cancellation) {
    mmltk::common::logging::ScopedProfile profile{"compiler.prepare"};
    if (splits.empty()) { throw std::runtime_error("dataset compile plan requires at least one split"); }
    if (const auto valid = validate_compiler_config(config); !valid) {
        throw std::runtime_error(std::string(compiler_config_validation_message(valid.error())));
    }
    if (config.worker_cpus.empty()) { config.worker_cpus = allowed_cpu_set(); }
    if (config.worker_cpus.empty()) { throw std::runtime_error("dataset compilation requires at least one allowed CPU"); }
    compiler_internal::DatasetScan scan = compiler_internal::scan_dataset(config, splits, cancellation);
    DatasetCompilePlan plan;
    plan.config = std::move(config);
    plan.class_map = std::move(scan.class_map);
    plan.splits = std::move(scan.splits);
    return plan;
}

void DatasetCompiler::compile(const DatasetCompilePlan& plan, const size_t split_index, CompileTelemetry* const telemetry,
                              const mmltk::common::concurrency::CancellationObservation cancellation) {
    mmltk::common::logging::ScopedProfile profile{"compiler.total"};
    if (split_index >= plan.splits.size()) { throw std::runtime_error("dataset compile split index is out of range"); }
    CompilerConfig effective_config = plan.config;
    effective_config.split = plan.splits[split_index].split;
    if (const auto valid = validate_compiler_config(effective_config); !valid) {
        throw std::runtime_error(std::string(compiler_config_validation_message(valid.error())));
    }
    const auto throw_if_cancelled = [&] {
        if (cancellation.requested()) { throw std::runtime_error("dataset compilation cancelled"); }
    };
    throw_if_cancelled();
    std::atomic<bool> failure_requested{false};
    const std::filesystem::path split_dir = std::filesystem::path(effective_config.source_dir) / effective_config.split;
    const std::filesystem::path out_dir = effective_config.output_dir;
    std::filesystem::create_directories(out_dir);

    const uint32_t width = effective_config.target_width;
    const uint32_t height = effective_config.target_height;
    const uint32_t channels = 3;
    const size_t image_stride = static_cast<size_t>(channels) * height * width * sizeof(float);

    const int num_workers =
        compiler_internal::resolve_num_workers(effective_config.num_workers, std::span<const int>(effective_config.worker_cpus));
    const int initial_pixel_workers = num_workers == 1 ? 0 : (num_workers + 1) / 2;
    const int label_workers = num_workers - initial_pixel_workers;
    const int pixel_workers = num_workers;
    const std::span<const int> worker_cpus(effective_config.worker_cpus.data(), static_cast<size_t>(num_workers));
    const std::span<const int> label_cpus = worker_cpus.last(static_cast<size_t>(label_workers));
    mmltk::common::logging::profile_set_value("compiler.num_workers", static_cast<size_t>(num_workers));
    const auto& class_map = plan.class_map;
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
        log.debug(
            "[compile] {}: {} images, {} workers ({} pixel initial + {} label, {} pixel after handoff), target {}x{}, "
            "stride={} bytes/img",
            effective_config.split, num_images, num_workers, initial_pixel_workers, label_workers, pixel_workers, width, height,
            image_stride);
    });

    compiler_internal::FileLayout layout;
    {
        mmltk::common::logging::ScopedProfile layout_profile{"compiler.compute_pixel_layout"};
        layout = compiler_internal::compute_pixel_layout(num_images, image_stride);
    }

    mmltk::common::logging::debug([&](spdlog::logger& log) {
        log.debug("[compile] Pixel layout: pixel_offset={} ({:.1f} MB aligned), pixels={:.2f} GB", layout.pixel_offset,
                  static_cast<double>(layout.pixel_offset) / (1024.0 * 1024.0),
                  static_cast<double>(layout.pixel_blob_size) / (1024.0 * 1024.0 * 1024.0));
    });

    const std::filesystem::path out_path = out_dir / (effective_config.split + ".bin");
    std::string staging_path_text = out_path.string() + ".tmp.XXXXXX";
    FileHandle fd;
    {
        mmltk::common::logging::ScopedProfile output_profile{"compiler.open_output"};
        fd = FileHandle::create_unique_output(staging_path_text, layout.pixel_offset + layout.pixel_blob_size);
    }
    const std::filesystem::path staging_path(staging_path_text);
    StagingFileCleanup staging_cleanup(staging_path);

    compiler_internal::LabelBlocks label_blocks;
    std::exception_ptr pixel_error;
    std::exception_ptr label_error;
    std::atomic<bool> release_all_pixel_workers{false};

    std::thread pixel_thread([&] {
        try {
            compiler_internal::write_pixel_blob(fd, compiler_internal::PixelBlobWriteRequest{
                                                        split_dir,
                                                        num_images,
                                                        width,
                                                        height,
                                                        image_stride,
                                                        pixel_workers,
                                                        true,
                                                        true,
                                                        effective_config.perceptual_downscale,
                                                        telemetry != nullptr ? &pixel_progress : nullptr,
                                                        worker_cpus,
                                                        initial_pixel_workers,
                                                        &release_all_pixel_workers,
                                                        layout.pixel_offset,
                                                        cancellation,
                                                        &failure_requested,
                                                    });
        } catch (...) {
            failure_requested.store(true, std::memory_order_relaxed);
            pixel_error = std::current_exception();
        }
    });

    try {
        label_blocks =
            compiler_internal::build_label_blocks(split_dir, num_images, effective_config, class_map, label_workers, label_cpus,
                                                  telemetry != nullptr ? &label_progress : nullptr, &failure_requested, cancellation);
        if (telemetry != nullptr) { telemetry->set_dropped_instances(label_blocks.dropped_instances); }
    } catch (...) {
        failure_requested.store(true, std::memory_order_relaxed);
        label_error = std::current_exception();
    }

    release_all_pixel_workers.store(true, std::memory_order_release);
    release_all_pixel_workers.notify_all();
    pixel_thread.join();
    if (pixel_error) { std::rethrow_exception(pixel_error); }
    if (label_error) { std::rethrow_exception(label_error); }
    throw_if_cancelled();
    mmltk::common::logging::debug([](spdlog::logger& log) { log.debug("[compile] Labels and pixels complete"); });

    {
        mmltk::common::logging::ScopedProfile layout_profile{"compiler.finalize_layout"};
        compiler_internal::finalize_layout(layout, compiler_internal::LayoutFinalizeInputs{
                                                       label_blocks.labels.size(),
                                                       label_blocks.rle_pairs.size(),
                                                   });
    }

    compiler_internal::assign_pixel_offsets(label_blocks.index, layout.pixel_offset, image_stride);
    throw_if_cancelled();

    mmltk::common::logging::debug([&](spdlog::logger& log) {
        log.debug("[compile] Layout: total={:.2f} GB", static_cast<double>(layout.total_size) / (1024.0 * 1024.0 * 1024.0));
    });

    fd.preallocate(layout.total_size);

    const FileHeader header = compiler_internal::make_file_header(
        compiler_internal::FileHeaderInputs{
            num_images,
            width,
            height,
            channels,
            label_blocks.max_instances_per_image,
            image_stride,
        },
        class_map, layout);
    validate_compiled_index_entries(label_blocks.index, header, label_blocks.labels.size(), cancellation);
    validate_compiled_original_image_dimensions(label_blocks.index, cancellation);
    const size_t used_rle_bytes = validate_compiled_label_entries(label_blocks.labels, header, layout.rle_block_size, cancellation);
    if (used_rle_bytes != layout.rle_block_size) {
        throw std::runtime_error("compiled RLE count does not match the referenced instance spans");
    }
    validate_compiled_rle_pairs(label_blocks.labels, label_blocks.rle_pairs, static_cast<size_t>(width) * height, cancellation);
    throw_if_cancelled();
    compiler_internal::write_metadata_blocks(fd, layout, header, label_blocks, cancellation);
    throw_if_cancelled();
    if (telemetry != nullptr) { telemetry->enter_syncing(); }
    fd.sync_data();
    throw_if_cancelled();
    fd = FileHandle{};
    const FileHandle staged_file = FileHandle::open_readonly(staging_path.string());
    const FileHeader staged_header = read_compiled_header(staged_file);
    (void)validate_compiled_file_sections(staged_header, staged_file.size());
    throw_if_cancelled();
    if (telemetry != nullptr) { telemetry->enter_publishing(); }
    throw_if_cancelled();
    publish_staged_path_atomically(staging_path, out_path);
    staging_cleanup.published();

    mmltk::common::logging::debug([&](spdlog::logger& log) {
        log.debug("[compile] Written {} ({:.2f} GB)", out_path.string(),
                  static_cast<double>(layout.total_size) / (1024.0 * 1024.0 * 1024.0));
    });
}

}  // namespace mmltk::backend::data
