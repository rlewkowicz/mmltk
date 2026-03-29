#include "debug_utils.h"
#include "dataset_compiler.h"
#include "dataset_compiler_internal.h"
#include "execution_policy.h"
#include "profile_utils.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fastloader {

namespace {

class CompileProgressReporter {
public:
    CompileProgressReporter(size_t num_images,
                            const std::function<void(const CompileProgress&)>& progress_cb,
                            compiler_internal::DroppedAnnotationTracker* dropped_annotations)
        : num_images_(num_images),
          total_steps_(num_images * 2 + 1),
          progress_cb_(progress_cb),
          dropped_annotations_(dropped_annotations) {
        if (progress_cb_) {
            label_done_.wake_cv = &wake_cv_;
            pixel_done_.wake_cv = &wake_cv_;
            if (dropped_annotations_ != nullptr) {
                dropped_annotations_->wake_cv = &wake_cv_;
            }
            reporter_thread_ = std::thread([this] { run(); });
            wait_for_startup();
        }
    }

    ~CompileProgressReporter() {
        stop_noexcept();
    }

    CompileProgressReporter(const CompileProgressReporter&) = delete;
    CompileProgressReporter& operator=(const CompileProgressReporter&) = delete;

    compiler_internal::ProgressCounter* label_counter() noexcept {
        return progress_cb_ ? &label_done_ : nullptr;
    }

    compiler_internal::ProgressCounter* pixel_counter() noexcept {
        return progress_cb_ ? &pixel_done_ : nullptr;
    }

    void enter_syncing() {
        if (!reporter_thread_.joinable()) {
            return;
        }
        phase_.store(CompileProgressPhase::kSyncing, std::memory_order_release);
        wake();
        rethrow_if_failed();
    }

    void rethrow_if_failed() {
        std::lock_guard<std::mutex> lock(error_mutex_);
        if (error_ != nullptr) {
            std::rethrow_exception(error_);
        }
    }

    void finish() {
        if (!reporter_thread_.joinable()) {
            return;
        }
        stop_requested_.store(true, std::memory_order_release);
        wake();
        reporter_thread_.join();
        rethrow_if_failed();
    }

private:
    static constexpr auto kHeartbeatInterval = std::chrono::milliseconds(100);

    void wait_for_startup() {
        std::unique_lock<std::mutex> lock(startup_mutex_);
        startup_cv_.wait(lock, [&] { return startup_complete_; });
        rethrow_if_failed();
    }

    void mark_startup_complete() {
        {
            std::lock_guard<std::mutex> lock(startup_mutex_);
            startup_complete_ = true;
        }
        startup_cv_.notify_all();
    }

    void run() {
        try {
            apply_worker_execution_policy(ExecutionPolicyRequest{
                {},
                "fl_cmprpt",
                0,
                true,
                true,
            });
            CompileProgress last_snapshot = make_snapshot();
            progress_cb_(last_snapshot);
            mark_startup_complete();
            auto next_heartbeat = std::chrono::steady_clock::now() + kHeartbeatInterval;
            while (true) {
                std::unique_lock<std::mutex> lock(wake_mutex_);
                wake_cv_.wait_until(lock, next_heartbeat, [&] {
                    return stop_requested_.load(std::memory_order_acquire) ||
                           snapshot_changed(make_snapshot(), last_snapshot);
                });
                lock.unlock();
                const auto now = std::chrono::steady_clock::now();
                const CompileProgress current_snapshot = make_snapshot();
                const bool changed = snapshot_changed(current_snapshot, last_snapshot);
                if (changed || now >= next_heartbeat) {
                    progress_cb_(current_snapshot);
                    last_snapshot = current_snapshot;
                    next_heartbeat = now + kHeartbeatInterval;
                }
                if (stop_requested_.load(std::memory_order_acquire) &&
                    !snapshot_changed(make_snapshot(), last_snapshot)) {
                    break;
                }
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                if (error_ == nullptr) {
                    error_ = std::current_exception();
                }
            }
            mark_startup_complete();
            stop_requested_.store(true, std::memory_order_release);
            wake();
        }
    }

    CompileProgress make_snapshot() const noexcept {
        const CompileProgressPhase explicit_phase = phase_.load(std::memory_order_acquire);
        if (explicit_phase == CompileProgressPhase::kSyncing) {
            return CompileProgress{
                total_steps_,
                total_steps_,
                CompileProgressPhase::kSyncing,
                0,
                dropped_annotations_ != nullptr ? dropped_annotations_->load() : 0,
            };
        }
        const size_t label_d = label_done_.load();
        const size_t pixel_d = pixel_done_.load();
        const size_t done = label_d + pixel_d;
        const size_t active = pixel_done_.active_load();
        const size_t dropped_annotations = dropped_annotations_ != nullptr ? dropped_annotations_->load() : 0;
        // Prefer an explicit pixel phase while pixel workers are active so
        // progress remains stable even when labels finish very quickly.
        CompileProgressPhase phase;
        if (pixel_d < num_images_ && active > 0) {
            phase = CompileProgressPhase::kPixels;
        } else if (pixel_d >= num_images_) {
            phase = CompileProgressPhase::kLabels;
        } else if (label_d >= num_images_) {
            phase = CompileProgressPhase::kPixels;
        } else {
            phase = (label_d <= pixel_d) ? CompileProgressPhase::kLabels
                                         : CompileProgressPhase::kPixels;
        }
        return CompileProgress{done, total_steps_, phase, active, dropped_annotations};
    }

    static bool snapshot_changed(const CompileProgress& left, const CompileProgress& right) noexcept {
        return left.done != right.done ||
               left.phase != right.phase ||
               left.active != right.active ||
               left.dropped_annotations != right.dropped_annotations ||
               left.total != right.total;
    }

    void wake() noexcept {
        wake_cv_.notify_one();
    }

    void stop_noexcept() noexcept {
        if (!reporter_thread_.joinable()) {
            return;
        }
        stop_requested_.store(true, std::memory_order_release);
        wake();
        reporter_thread_.join();
    }

    size_t num_images_ = 0;
    size_t total_steps_ = 0;
    std::function<void(const CompileProgress&)> progress_cb_;
    compiler_internal::ProgressCounter label_done_;
    compiler_internal::ProgressCounter pixel_done_;
    compiler_internal::DroppedAnnotationTracker* dropped_annotations_ = nullptr;
    std::atomic<CompileProgressPhase> phase_{CompileProgressPhase::kLabels};
    std::atomic<bool> stop_requested_{false};
    std::thread reporter_thread_;
    std::condition_variable startup_cv_;
    std::mutex startup_mutex_;
    bool startup_complete_ = false;
    std::condition_variable wake_cv_;
    std::mutex wake_mutex_;
    std::mutex error_mutex_;
    std::exception_ptr error_;
};

} // namespace

void DatasetCompiler::compile(const CompilerConfig& config,
                              const std::function<void(const CompileProgress&)>& progress_cb) {
    FASTLOADER_PROFILE_SCOPE("compiler.total");
    const std::filesystem::path split_dir = std::filesystem::path(config.source_dir) / config.split;
    const std::filesystem::path out_dir = config.output_dir;
    std::filesystem::create_directories(out_dir);

    const uint32_t width = config.target_width;
    const uint32_t height = config.target_height;
    const uint32_t channels = 3;
    const size_t image_stride = static_cast<size_t>(channels) * height * width * sizeof(float);

    const int num_workers = compiler_internal::resolve_num_workers(config.num_workers);
    const int pixel_workers = std::max(1, (num_workers + 1) / 2);
    const int label_workers = std::max(1, num_workers - pixel_workers);
    FASTLOADER_PROFILE_SET("compiler.num_workers", static_cast<size_t>(num_workers));
    if (config.cuda_mask_batch_size < 0) {
        throw std::runtime_error("cuda_mask_batch_size must be non-negative");
    }

    compiler_internal::DatasetScan scan;
    {
        FASTLOADER_PROFILE_SCOPE("compiler.scan_dataset");
        scan = compiler_internal::scan_dataset(config);
    }
    const auto& class_map = scan.class_map;
    const uint32_t num_images = scan.num_images;
    FASTLOADER_PROFILE_SET("compiler.num_images", num_images);
    FASTLOADER_PROFILE_SET("compiler.image_stride_bytes", image_stride);
    compiler_internal::DroppedAnnotationTracker dropped_annotations;
    CompileProgressReporter progress_reporter(num_images, progress_cb, &dropped_annotations);

    FASTLOADER_DEBUG_LOG("[compile] %s: %u images, %d workers (%d pixel + %d label), target %ux%u, stride=%zu bytes/img\n",
                         config.split.c_str(), num_images, num_workers, pixel_workers, label_workers, width, height, image_stride);

    // Phase 1: compute pixel layout (known upfront — only needs num_images + stride)
    compiler_internal::FileLayout layout;
    {
        FASTLOADER_PROFILE_SCOPE("compiler.compute_pixel_layout");
        layout = compiler_internal::compute_pixel_layout(num_images, image_stride);
    }

    FASTLOADER_DEBUG_LOG("[compile] Pixel layout: pixel_offset=%zu (%.1f MB aligned), pixels=%.2f GB\n",
                         layout.pixel_offset,
                         static_cast<double>(layout.pixel_offset) / (1024.0 * 1024.0),
                         static_cast<double>(layout.pixel_blob_size) / (1024.0 * 1024.0 * 1024.0));

    // Create output file sized for header+index+padding+pixels (will extend later for labels/rle)
    const std::string out_path = (out_dir / (config.split + ".bin")).string();
    FileHandle fd;
    {
        FASTLOADER_PROFILE_SCOPE("compiler.open_output");
        fd = FileHandle::create_output(out_path, layout.pixel_offset + layout.pixel_blob_size);
    }

    // Run labels and pixels concurrently
    compiler_internal::LabelBlocks label_blocks;
    std::exception_ptr pixel_error;
    std::exception_ptr label_error;

    std::thread pixel_thread([&] {
        try {
            compiler_internal::write_pixel_blob(fd,
                                                split_dir,
                                                num_images,
                                                width,
                                                height,
                                                image_stride,
                                                pixel_workers,
                                                /*any_resize=*/true,
                                                /*any_downscale=*/true,
                                                progress_reporter.pixel_counter(),
                                                layout.pixel_offset);
        } catch (...) {
            pixel_error = std::current_exception();
        }
    });

    try {
        label_blocks = compiler_internal::build_label_blocks(split_dir,
                                                             num_images,
                                                             config,
                                                             class_map,
                                                             label_workers,
                                                             progress_reporter.label_counter(),
                                                             &dropped_annotations);
    } catch (...) {
        label_error = std::current_exception();
    }

    pixel_thread.join();
    progress_reporter.rethrow_if_failed();
    if (label_error) {
        std::rethrow_exception(label_error);
    }
    if (pixel_error) {
        std::rethrow_exception(pixel_error);
    }
    FASTLOADER_DEBUG_LOG("[compile] Labels and pixels complete\n");

    // Phase 2: finalize layout with actual label/rle counts
    {
        FASTLOADER_PROFILE_SCOPE("compiler.finalize_layout");
        compiler_internal::finalize_layout(layout,
                                           label_blocks.labels.size(),
                                           label_blocks.rle_pairs.size());
    }

    compiler_internal::assign_pixel_offsets(label_blocks.index, layout.pixel_offset, image_stride);

    FASTLOADER_DEBUG_LOG("[compile] Layout: total=%.2f GB\n",
                         static_cast<double>(layout.total_size) / (1024.0 * 1024.0 * 1024.0));

    // Extend file to final size for labels + rle
    fd.preallocate(layout.total_size);

    const FileHeader header = compiler_internal::make_file_header(num_images,
                                                                  width,
                                                                  height,
                                                                  channels,
                                                                  image_stride,
                                                                  class_map,
                                                                  layout);
    compiler_internal::write_metadata_blocks(fd, layout, header, label_blocks);
    progress_reporter.enter_syncing();
    fd.sync_data();
    progress_reporter.finish();

    if (label_blocks.dropped_annotations > 0) {
        std::fprintf(stderr,
                     "\r\033[2Kcompile %s: dropped %zu annotations whose masks fully vanished during resize\n",
                     config.split.c_str(),
                     label_blocks.dropped_annotations);
        for (const std::string& sample : label_blocks.dropped_annotation_examples) {
            std::fprintf(stderr, "  sample: %s\n", sample.c_str());
        }
        if (label_blocks.dropped_annotations > label_blocks.dropped_annotation_examples.size()) {
            std::fprintf(stderr,
                         "  ... %zu more omitted\n",
                         label_blocks.dropped_annotations - label_blocks.dropped_annotation_examples.size());
        }
        std::fflush(stderr);
    }

    FASTLOADER_DEBUG_LOG("[compile] Written %s (%.2f GB)\n", out_path.c_str(),
                         static_cast<double>(layout.total_size) / (1024.0 * 1024.0 * 1024.0));
}

} // namespace fastloader
