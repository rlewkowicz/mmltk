#include "mmltk/live/static_workspace_compositor.h"

#include "live/live_helpers.h"
#include "live/live_worker_logging.h"
#include "mmltk/live/live_manual_overlay_worker.h"
#include "mmltk/live/live_worker_runtime.h"
#include "mmltk/live/workspace_surface_pool.h"
#include "mmltk/live/workspace_trace.h"
#include "mmltk/rfdetr/draw_cuda.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mmltk::live {

namespace {

[[nodiscard]] mmltk::rfdetr::draw_launch::WorkspaceRgbaTarget target_for_lease(const WorkspaceSurfaceWriteLease& lease,
                                                                               const int width, const int height) {
    if (lease.pitched) {
        return mmltk::rfdetr::draw_launch::make_workspace_rgba_pitched_target(device_ptr_as_bytes(lease.data),
                                                                              lease.pitch_bytes, width, height);
    }
    if (lease.array) {
        return mmltk::rfdetr::draw_launch::make_workspace_rgba_surface_target(lease.surface_object, width, height);
    }
    throw std::runtime_error("static workspace surface has no CUDA target");
}

bool enqueue_overlay_release_barrier(const cudaEvent_t consumed_event, const cudaStream_t consumer_stream,
                                     const cudaStream_t producer_stream) noexcept {
    if (consumed_event == nullptr || consumer_stream == nullptr || producer_stream == nullptr) {
        log_live_worker_message("static.workspace", "error",
                                "cannot release consumed overlay without CUDA stream and event handles");
        return false;
    }
    cudaError_t result = cudaEventRecord(consumed_event, consumer_stream);
    if (result == cudaSuccess) {
        result = cudaStreamWaitEvent(producer_stream, consumed_event, 0U);
    }
    if (result != cudaSuccess) {
        log_live_worker_message(
            "static.workspace", "error",
            std::string("failed to enqueue overlay input-release barrier: ") + cudaGetErrorString(result));
        return false;
    }
    return true;
}

}  

struct StaticWorkspaceCompositor::Impl {
    Impl(std::shared_ptr<WorkspaceSurfacePool> pool_in, const int cuda_device_index_in,
         const std::uint32_t max_width_in, const std::uint32_t max_height_in, const std::uint32_t overlay_slot_count,
         const std::uint32_t overlay_canvas_width_in, const std::uint32_t overlay_canvas_height_in)
        : pool(std::move(pool_in)),
          cuda_device_index(cuda_device_index_in),
          max_width(max_width_in),
          max_height(max_height_in),
          overlay_canvas_width(overlay_canvas_width_in == 0U ? max_width_in : overlay_canvas_width_in),
          overlay_canvas_height(overlay_canvas_height_in == 0U ? max_height_in : overlay_canvas_height_in),
          overlay_worker(overlay_document, cuda_device_index, overlay_canvas_width, overlay_canvas_height,
                         overlay_slot_count) {
        if (pool == nullptr || max_width == 0U || max_height == 0U) {
            throw std::runtime_error("static workspace compositor requires a pool and non-zero dimensions");
        }
        ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for static workspace compositor");
        source_device.ensure_dimensions(max_width, max_height, "cudaMallocPitch for static workspace source");
        source_stream.create_with_highest_priority("cudaStreamCreateWithPriority for static workspace source");
        source_ready.create(cudaEventDisableTiming, "cudaEventCreateWithFlags for static workspace source");
        overlay_consumed.create(cudaEventDisableTiming,
                                "cudaEventCreateWithFlags for static workspace overlay consumption");
    }

    ~Impl() {
        try {
            stop();
        } catch (...) {
            running.store(false, std::memory_order_release);
        }
        (void)cudaSetDevice(cuda_device_index);
        overlay_consumed.reset();
        source_ready.reset();
        source_stream.reset();
        source_device.reset();
        source_upload.reset();
    }

    void start() {
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            overlay_worker.start();
        } catch (...) {
            running.store(false, std::memory_order_release);
            throw;
        }
    }

    void stop() {
        if (!running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        overlay_worker.stop();
        pool->reset_for_producer_restart();
    }

    void set_source(const std::uint8_t* pixels, const std::size_t byte_count, const std::uint32_t width_in,
                    const std::uint32_t height_in, const LiveFrameId frame_id_in, std::string source_name_in,
                    LiveCaptureRegion source_region_in) {
        if (pixels == nullptr || width_in == 0U || height_in == 0U || width_in > max_width || height_in > max_height) {
            throw std::runtime_error("static workspace source dimensions are invalid");
        }
        if (width_in > std::numeric_limits<std::size_t>::max() / 3U ||
            static_cast<std::size_t>(width_in) * 3U >
                std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(height_in)) {
            throw std::overflow_error("static workspace source size overflow");
        }
        const std::size_t row_bytes = static_cast<std::size_t>(width_in) * 3U;
        const std::size_t required_bytes = row_bytes * static_cast<std::size_t>(height_in);
        if (byte_count != required_bytes) {
            throw std::runtime_error("static workspace source byte count does not match dimensions");
        }
        if (source_region_in.width == 0U || source_region_in.height == 0U) {
            source_region_in = LiveCaptureRegion{0U, 0U, width_in, height_in};
        }
        if (source_region_in.width != width_in || source_region_in.height != height_in) {
            throw std::runtime_error("static workspace source region dimensions do not match the source image");
        }
        if (static_cast<std::uint64_t>(source_region_in.x) + source_region_in.width > overlay_canvas_width ||
            static_cast<std::uint64_t>(source_region_in.y) + source_region_in.height > overlay_canvas_height) {
            throw std::runtime_error("static workspace source region exceeds the overlay canvas");
        }

        std::lock_guard<std::mutex> lock(source_mutex);
        if (source_revision.load(std::memory_order_relaxed) != 0U) {
            throw std::runtime_error(
                "static workspace compositor source is immutable; create a new compositor for a new source");
        }
        ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for static workspace source upload");
        source_upload.ensure_capacity(required_bytes, "cudaHostAlloc for static workspace source upload");
        std::memcpy(source_upload.data(), pixels, required_bytes);
        ensure_cuda_ok(cudaMemcpy2DAsync(device_ptr_as_void(source_device.data()), source_device.pitch_bytes(),
                                         source_upload.data(), row_bytes, row_bytes, height_in, cudaMemcpyHostToDevice,
                                         source_stream.get()),
                       "cudaMemcpy2DAsync for static workspace source upload");
        ensure_cuda_ok(cudaEventRecord(source_ready.get(), source_stream.get()),
                       "cudaEventRecord for static workspace source upload");
        width = width_in;
        height = height_in;
        frame_id = frame_id_in;
        source_region = source_region_in;
        source_name = std::move(source_name_in);
        source_revision.fetch_add(1U, std::memory_order_release);
        dirty.store(true, std::memory_order_release);
        trace_workspace("native", "workspace.static_source_uploaded", [&] {
            return nlohmann::json{
                {"generation", pool->generation()}, {"width", width}, {"height", height}, {"bytes", required_bytes}};
        });
        publish_error({});
    }

    [[nodiscard]] bool refresh() {
        if (!running.load(std::memory_order_acquire)) {
            return false;
        }

        LiveManualOverlayWorker::OverlayView overlay{};
        WorkspaceSurfaceWriteLease write{};
        cudaStream_t consumer_stream = nullptr;
        const bool overlay_acquired = overlay_worker.try_acquire_latest_overlay(&overlay);
        bool overlay_was_consumed = false;
        auto overlay_release = worker_runtime::make_scoped_rollback(
            [this, &overlay, &consumer_stream, overlay_acquired, &overlay_was_consumed]() noexcept {
                if (overlay_acquired) {
                    if (overlay_was_consumed && !enqueue_overlay_release_barrier(
                                                    overlay_consumed.get(), consumer_stream, overlay.producer_stream)) {
                        return;
                    }
                    try {
                        overlay_worker.release_overlay(overlay.slot_index);
                    } catch (...) {
                        dirty.store(true, std::memory_order_release);
                    }
                }
            });
        const bool overlay_changed = overlay_acquired && overlay.generation != rendered_overlay_generation;
        const bool sync_ready = pool->timeline_sync_ready(pool->generation());
        const std::uint64_t presentation_epoch = pool->presentation_epoch();
        const std::uint64_t availability_epoch = pool->availability_epoch();
        const std::uint64_t current_source_revision = source_revision.load(std::memory_order_acquire);
        if (presentation_epoch != rendered_presentation_epoch) {
            dirty.store(true, std::memory_order_release);
        }
        if (sync_ready && !published_after_sync) {
            dirty.store(true, std::memory_order_release);
        }
        if (!dirty.load(std::memory_order_acquire) && !overlay_changed) {
            return false;
        }
        const std::uint64_t desired_overlay_generation = overlay_acquired ? overlay.generation : 0U;
        if (pressure_blocked && pressure_availability_epoch == availability_epoch &&
            pressure_source_revision == current_source_revision &&
            pressure_overlay_generation == desired_overlay_generation &&
            pressure_presentation_epoch == presentation_epoch && pressure_sync_ready == sync_ready) {
            return false;
        }

        std::lock_guard<std::mutex> lock(source_mutex);
        if (width == 0U || height == 0U) {
            return false;
        }
        if (!pool->try_reserve_write(&write)) {
            pressure_blocked = true;
            pressure_availability_epoch = availability_epoch;
            pressure_source_revision = current_source_revision;
            pressure_overlay_generation = desired_overlay_generation;
            pressure_presentation_epoch = presentation_epoch;
            pressure_sync_ready = sync_ready;
            trace_workspace("native", "workspace.static_refresh_dropped", [&] {
                return nlohmann::json{{"generation", pool->generation()}, {"reason", "surface_pressure"}};
            });
            return false;
        }
        consumer_stream = write.stream;
        auto write_cancel =
            worker_runtime::make_scoped_rollback([this, &write]() noexcept { pool->cancel_write(&write); });
        try {
            ensure_cuda_ok(cudaStreamWaitEvent(write.stream, source_ready.get(), 0),
                           "cudaStreamWaitEvent for static workspace source");
            const int image_width = static_cast<int>(width);
            const int image_height = static_cast<int>(height);
            const auto target = target_for_lease(write, image_width, image_height);
            mmltk::rfdetr::launch_copy_bgr_to_workspace_rgba(device_ptr_as_bytes(source_device.data()),
                                                             source_device.pitch_bytes(), target, image_width,
                                                             image_height, 255U, write.stream);
            ensure_cuda_ok(cudaPeekAtLastError(), "launch_copy_bgr_to_workspace_rgba for static workspace");
            if (overlay_acquired && overlay.has_content) {
                ensure_cuda_ok(cudaStreamWaitEvent(write.stream, overlay.ready_event, 0),
                               "cudaStreamWaitEvent for static workspace overlay");
                overlay_was_consumed = true;
                const std::uint64_t overlay_right = static_cast<std::uint64_t>(source_region.x) + source_region.width;
                const std::uint64_t overlay_bottom = static_cast<std::uint64_t>(source_region.y) + source_region.height;
                if (overlay_right > overlay.width || overlay_bottom > overlay.height) {
                    throw std::runtime_error("static overlay does not cover the source region");
                }
                const CUdeviceptr overlay_region = overlay.data +
                                                   static_cast<std::size_t>(source_region.y) * overlay.pitch_bytes +
                                                   static_cast<std::size_t>(source_region.x) * 4U;
                mmltk::rfdetr::launch_composite_rgba_over_workspace_rgba(target, device_ptr_as_bytes(overlay_region),
                                                                         overlay.pitch_bytes, image_width, image_height,
                                                                         write.stream);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch_composite_rgba_over_workspace_rgba for static workspace");
            }
            const std::uint64_t ready_ns = live_steady_clock_now_ns();
            const WorkspacePresentSnapshot published =
                pool->publish_write(&write, WorkspaceSurfacePublishInfo{
                                                frame_id,
                                                width,
                                                height,
                                                source_region,
                                                LiveCaptureRegion{0U, 0U, width, height},
                                                0U,
                                                ready_ns,
                                                false,
                                            });
            write_cancel.dismiss();
            trace_workspace("native", "workspace.static_composited", [&] {
                return nlohmann::json{{"generation", published.swapchain_generation},
                                      {"slot", published.front_slot_index},
                                      {"revision", published.revision},
                                      {"overlay_generation", overlay_acquired ? overlay.generation : 0U},
                                      {"ready_ns", ready_ns}};
            });
            if (overlay_acquired) {
                rendered_overlay_generation = overlay.generation;
            }
            rendered_source_revision = source_revision.load(std::memory_order_acquire);
            rendered_presentation_epoch = presentation_epoch;
            dirty.store(false, std::memory_order_release);
            published_after_sync = sync_ready;
            pressure_blocked = false;
            publish_error({});
            return true;
        } catch (const std::exception& error) {
            publish_error(error.what());
            throw;
        }
    }

    void publish_error(std::string message) {
        auto value = std::make_shared<const std::string>(std::move(message));
        last_error.store(std::move(value), std::memory_order_release);
    }

    std::shared_ptr<WorkspaceSurfacePool> pool;
    int cuda_device_index = 0;
    std::uint32_t max_width = 0U;
    std::uint32_t max_height = 0U;
    std::uint32_t overlay_canvas_width = 0U;
    std::uint32_t overlay_canvas_height = 0U;
    ManualOverlayDocument overlay_document;
    LiveManualOverlayWorker overlay_worker;
    BgrPitchedDeviceBuffer source_device;
    PinnedUploadBuffer<std::uint8_t> source_upload;
    CudaStreamHandle source_stream;
    CudaEventHandle source_ready;
    CudaEventHandle overlay_consumed;
    std::mutex source_mutex;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    LiveFrameId frame_id{};
    LiveCaptureRegion source_region{};
    std::string source_name;
    std::atomic<std::uint64_t> source_revision{0U};
    std::uint64_t rendered_source_revision = 0U;
    std::uint64_t rendered_overlay_generation = 0U;
    std::uint64_t rendered_presentation_epoch = 0U;
    std::uint64_t pressure_availability_epoch = 0U;
    std::uint64_t pressure_source_revision = 0U;
    std::uint64_t pressure_overlay_generation = 0U;
    std::uint64_t pressure_presentation_epoch = 0U;
    bool pressure_sync_ready = false;
    bool pressure_blocked = false;
    bool published_after_sync = false;
    std::atomic<bool> dirty{false};
    std::atomic<bool> running{false};
    std::atomic<std::shared_ptr<const std::string>> last_error{std::make_shared<const std::string>()};
};

StaticWorkspaceCompositor::StaticWorkspaceCompositor(std::shared_ptr<WorkspaceSurfacePool> workspace_pool,
                                                     const int cuda_device_index, const std::uint32_t max_width,
                                                     const std::uint32_t max_height,
                                                     const std::uint32_t overlay_slot_count,
                                                     const std::uint32_t overlay_canvas_width,
                                                     const std::uint32_t overlay_canvas_height)
    : impl_(std::make_unique<Impl>(std::move(workspace_pool), cuda_device_index, max_width, max_height,
                                   overlay_slot_count, overlay_canvas_width, overlay_canvas_height)) {}

StaticWorkspaceCompositor::~StaticWorkspaceCompositor() = default;

void StaticWorkspaceCompositor::start() {
    impl_->start();
}

void StaticWorkspaceCompositor::stop() {
    impl_->stop();
}

bool StaticWorkspaceCompositor::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

void StaticWorkspaceCompositor::set_source_bgr(const std::uint8_t* pixels_bgr, const std::size_t byte_count,
                                               const std::uint32_t width, const std::uint32_t height,
                                               const LiveFrameId frame_id, std::string source_name,
                                               const LiveCaptureRegion source_region) {
    impl_->set_source(pixels_bgr, byte_count, width, height, frame_id, std::move(source_name), source_region);
}

void StaticWorkspaceCompositor::clear_source() noexcept {
    std::lock_guard<std::mutex> lock(impl_->source_mutex);
    impl_->width = 0U;
    impl_->height = 0U;
    impl_->frame_id = {};
    impl_->source_region = {};
    impl_->source_name.clear();
    impl_->dirty.store(false, std::memory_order_release);
    impl_->pool->reset_for_producer_restart();
}

bool StaticWorkspaceCompositor::refresh() {
    return impl_->refresh();
}

ManualOverlayDocument& StaticWorkspaceCompositor::overlay_document() noexcept {
    return impl_->overlay_document;
}

const ManualOverlayDocument& StaticWorkspaceCompositor::overlay_document() const noexcept {
    return impl_->overlay_document;
}

std::string StaticWorkspaceCompositor::last_error() const {
    const std::shared_ptr<const std::string> error = impl_->last_error.load(std::memory_order_acquire);
    return error != nullptr ? *error : std::string{};
}

}  
