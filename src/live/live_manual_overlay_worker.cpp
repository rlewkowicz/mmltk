#include "mmltk/live/live_manual_overlay_worker.h"

#include "live/upload_buffer_utils.h"
#include "mmltk/live/live_worker_runtime.h"
#include "mmltk/live/manual_overlay_document.h"
#include "mmltk/rfdetr/draw_cuda.h"
#include "mmltk_logging.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace mmltk::live {

namespace {

void log_live_manual_overlay_message(const char* level, const std::string& message) {
    if (message.empty()) {
        return;
    }
    auto logger = mmltk::logging::logger("live.manual_overlay");
    if (std::string_view(level) == "error") {
        logger->error("{}", message);
    } else if (std::string_view(level) == "warn") {
        logger->warn("{}", message);
    } else {
        logger->info("{}", message);
    }
}

std::array<std::uint8_t, 3> category_color(const std::size_t index) {
    static constexpr std::array<std::array<std::uint8_t, 3>, 8> palette{{
        {{240, 196, 68}},
        {{88, 188, 255}},
        {{255, 128, 88}},
        {{96, 214, 146}},
        {{214, 112, 255}},
        {{255, 96, 152}},
        {{180, 214, 92}},
        {{255, 168, 64}},
    }};
    return palette[index % palette.size()];
}

bool box_has_area(const ManualOverlayBox& box) {
    return box.x2 > box.x1 && box.y2 > box.y1;
}

struct OverlayTarget {
    std::uint8_t* data;
    std::size_t pitch;
    int width;
    int height;
    cudaStream_t stream;
};

void draw_manual_mask(const OverlayTarget& target, const ManualOverlayInstance& instance,
                      const std::array<std::uint8_t, 3>& color, const std::uint8_t alpha, const CUdeviceptr mask_data) {
    std::uint8_t* overlay_region = target.data +
                                   static_cast<std::size_t>(instance.mask_region.capture_y) * target.pitch +
                                   static_cast<std::size_t>(instance.mask_region.capture_x) * 4U;
    mmltk::rfdetr::launch_draw_manual_mask_rgba_pitched(
        overlay_region, target.pitch, static_cast<int>(instance.mask_region.width),
        static_cast<int>(instance.mask_region.height), device_ptr_as_bytes(mask_data), color[0], color[1], color[2],
        alpha, target.stream);
}

void draw_manual_mask_runs(const OverlayTarget& target, const ManualOverlayInstance& instance,
                           const std::array<std::uint8_t, 3>& color, const std::uint8_t alpha,
                           const CUdeviceptr runs_data) {
    std::uint8_t* overlay_region = target.data +
                                   static_cast<std::size_t>(instance.mask_region.capture_y) * target.pitch +
                                   static_cast<std::size_t>(instance.mask_region.capture_x) * 4U;
    mmltk::rfdetr::launch_draw_manual_mask_runs_rgba_pitched(
        overlay_region, target.pitch, static_cast<int>(instance.mask_region.width),
        static_cast<int>(instance.mask_region.height),
        reinterpret_cast<const std::uint32_t*>(device_ptr_as_const_void(runs_data)),
        static_cast<std::uint32_t>(instance.mask_runs.size()), color[0], color[1], color[2], alpha, target.stream);
}

void draw_polyline(const OverlayTarget& target, const ManualOverlayInstance& instance,
                   const std::array<std::uint8_t, 3>& color, const int thickness, const CUdeviceptr points_data) {
    mmltk::rfdetr::launch_draw_polyline_rgba_pitched(target.data, target.pitch, target.width, target.height,
                                                     reinterpret_cast<const int*>(device_ptr_as_void(points_data)),
                                                     static_cast<int>(instance.polyline_points.size()),
                                                     instance.polyline_closed, color[0], color[1], color[2], thickness,
                                                     target.stream);
}

void draw_skeleton(const OverlayTarget& target, const ManualOverlayInstance& instance,
                   const std::array<std::uint8_t, 3>& color, const int thickness, const CUdeviceptr points_data,
                   const CUdeviceptr edges_data) {
    mmltk::rfdetr::launch_draw_skeleton_rgba_pitched(
        target.data, target.pitch, target.width, target.height,
        reinterpret_cast<const int*>(device_ptr_as_void(points_data)), static_cast<int>(instance.points.size()),
        reinterpret_cast<const std::uint32_t*>(device_ptr_as_void(edges_data)),
        static_cast<int>(instance.skeleton_edges.size()), color[0], color[1], color[2], thickness, target.stream);
}

void draw_points(const OverlayTarget& target, const ManualOverlayInstance& instance,
                 const std::array<std::uint8_t, 3>& color, const int radius, const std::uint8_t alpha,
                 const CUdeviceptr points_data) {
    mmltk::rfdetr::launch_draw_points_rgba_pitched(target.data, target.pitch, target.width, target.height,
                                                   reinterpret_cast<const int*>(device_ptr_as_void(points_data)),
                                                   static_cast<int>(instance.points.size()), radius, color[0], color[1],
                                                   color[2], alpha, target.stream);
}

void draw_box_outline(const OverlayTarget& target, const ManualOverlayInstance& instance,
                      const std::array<std::uint8_t, 3>& color, const int thickness) {
    mmltk::rfdetr::launch_draw_box_outline_rgba_pitched(
        target.data, target.pitch, target.width, target.height, instance.box.x1, instance.box.y1, instance.box.x2,
        instance.box.y2, color[0], color[1], color[2], thickness, target.stream);
}

void draw_selection_handles(const OverlayTarget& target, const ManualOverlayInstance& selected,
                            const std::array<std::uint8_t, 3>& color, const int radius, const std::uint8_t alpha) {
    mmltk::rfdetr::launch_draw_selection_handles_rgba_pitched(
        target.data, target.pitch, target.width, target.height, selected.box.x1, selected.box.y1, selected.box.x2,
        selected.box.y2, radius, color[0], color[1], color[2], alpha, target.stream);
}

void draw_brush_preview(const OverlayTarget& target, const ManualOverlayBrushPreview& brush_preview,
                        PinnedUploadBuffer<int>& host_points_upload, DeviceUploadBuffer<int>& device_points_upload) {
    static constexpr int kBrushPreviewSegments = 64;
    static constexpr double kTwoPi = 6.28318530717958647692;
    static constexpr std::size_t kBrushPreviewValueCount = static_cast<std::size_t>(kBrushPreviewSegments) * 2U;

    ensure_pinned_upload_capacity<int>(kBrushPreviewValueCount, &host_points_upload,
                                       "cudaHostAlloc for manual overlay brush preview upload");
    ensure_device_upload_capacity<int>(kBrushPreviewValueCount, &device_points_upload,
                                       "cudaMalloc for manual overlay brush preview upload");

    int* points_xy = host_points_upload.data();
    const double radius = static_cast<double>(std::max(1, brush_preview.radius));
    for (int index = 0; index < kBrushPreviewSegments; ++index) {
        const double theta = kTwoPi * static_cast<double>(index) / static_cast<double>(kBrushPreviewSegments);
        const std::size_t point_offset = static_cast<std::size_t>(index) * 2U;
        points_xy[point_offset] =
            static_cast<int>(std::lround(static_cast<double>(brush_preview.capture_x) + std::cos(theta) * radius));
        points_xy[point_offset + 1U] =
            static_cast<int>(std::lround(static_cast<double>(brush_preview.capture_y) + std::sin(theta) * radius));
    }

    ensure_cuda_ok(cudaMemcpyAsync(device_ptr_as_void(device_points_upload.data()), points_xy,
                                   kBrushPreviewValueCount * sizeof(int), cudaMemcpyHostToDevice, target.stream),
                   "cudaMemcpyAsync for manual overlay brush preview upload");
    const std::uint8_t r = brush_preview.erase ? 255U : 128U;
    const std::uint8_t g = brush_preview.erase ? 128U : 255U;
    const std::uint8_t b = brush_preview.erase ? 128U : 170U;
    mmltk::rfdetr::launch_draw_polyline_rgba_pitched(
        target.data, target.pitch, target.width, target.height,
        reinterpret_cast<const int*>(device_ptr_as_void(device_points_upload.data())), kBrushPreviewSegments, true, r,
        g, b, 2, target.stream);
}

}  

struct LiveManualOverlayWorker::Impl {
    struct PackedInstance {
        std::size_t mask_offset = 0U;
        std::size_t run_value_offset = 0U;
        std::size_t polyline_value_offset = 0U;
        std::size_t point_value_offset = 0U;
        std::size_t edge_value_offset = 0U;
    };

    struct OverlaySlot {
        std::uint32_t slot_index = 0;
        RgbaPitchedDeviceBuffer device_buffer;
        CudaStreamHandle stream;
        CudaEventHandle ready_event;
        PinnedUploadBuffer<std::uint8_t> host_mask_upload;
        DeviceUploadBuffer<std::uint8_t> device_mask_upload;
        PinnedUploadBuffer<int> host_points_upload;
        DeviceUploadBuffer<int> device_points_upload;
        PinnedUploadBuffer<std::uint32_t> host_edges_upload;
        DeviceUploadBuffer<std::uint32_t> device_edges_upload;
        PinnedUploadBuffer<std::uint32_t> host_mask_runs_upload;
        DeviceUploadBuffer<std::uint32_t> device_mask_runs_upload;
        PinnedUploadBuffer<int> host_brush_points_upload;
        DeviceUploadBuffer<int> device_brush_points_upload;
        std::vector<PackedInstance> packed_instances;
        std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
        std::uint64_t generation = 0;
        bool has_content = false;

        [[nodiscard]] LiveManualOverlayWorker::OverlayView make_view() const noexcept {
            return LiveManualOverlayWorker::OverlayView{
                slot_index,
                device_buffer.data(),
                device_buffer.pitch_bytes(),
                device_buffer.width(),
                device_buffer.height(),
                ready_event.get(),
                stream.get(),
                has_content,
                generation,
            };
        }

        void reset_for_reuse() noexcept {
            generation = 0;
            has_content = false;
        }

        [[nodiscard]] cudaEvent_t ready_event_handle() const noexcept {
            return ready_event.get();
        }
    };

    Impl(ManualOverlayDocument& document_in, const int cuda_device_index_in, const std::uint32_t max_capture_width_in,
         const std::uint32_t max_capture_height_in, const std::uint32_t overlay_slot_count)
        : document(document_in),
          cuda_device_index(cuda_device_index_in),
          max_capture_width(std::max<std::uint32_t>(1U, max_capture_width_in)),
          max_capture_height(std::max<std::uint32_t>(1U, max_capture_height_in)) {
        if (overlay_slot_count == 0U) {
            throw std::runtime_error("live manual overlay worker requires at least one overlay slot");
        }
        overlay_slots.reserve(overlay_slot_count);
        allocate_resources();
    }

    ~Impl() {
        try {
            stop();
        } catch (const std::exception& error) {
            log_live_manual_overlay_message("error", error.what());
        }
        destroy_resources();
    }

    void start() {
        if (running.load(std::memory_order_acquire)) {
            throw std::runtime_error("live manual overlay worker is already running");
        }
        if (thread.joinable()) {
            throw std::runtime_error("live manual overlay worker must be stopped before restart");
        }

        publish_error({});
        worker_runtime::publish_status(&status, Status{});
        ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for live manual overlay start");
        worker_runtime::reset_slot_views_for_restart(overlay_slots, latest_overlay_index,
                                                     "cudaEventSynchronize for live manual overlay slot restart");
        worker_runtime::start_worker_thread(
            thread, stop_requested, running, [this]() { worker_thread_main(); },
            "live manual overlay worker is already running",
            "live manual overlay worker must be stopped before restart");
    }

    void stop() {
        stop_requested.store(true, std::memory_order_release);
        document.notify_waiters();
        if (thread.joinable()) {
            thread.join();
        }
        running.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool running_state() const noexcept {
        return running.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool try_acquire_latest_overlay(LiveManualOverlayWorker::OverlayView* out) {
        return worker_runtime::try_acquire_latest_running_slot_view(
            overlay_slots, latest_overlay_index, running, out, "live manual overlay worker requires an overlay output");
    }

    void release_overlay(const std::uint32_t slot_index) {
        worker_runtime::release_slot_by_index(
            overlay_slots, slot_index, "live manual overlay worker release index out of range",
            "live manual overlay worker release called for a slot that is not acquired");
    }

    [[nodiscard]] Status snapshot_status() const {
        return worker_runtime::snapshot_status(status);
    }

    void allocate_resources() {
        ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for live manual overlay resource allocation");

        try {
            worker_runtime::allocate_pitched_device_slots(
                overlay_slots, static_cast<std::uint32_t>(overlay_slots.capacity()), max_capture_width,
                max_capture_height, "cudaMallocPitch for live manual overlay",
                "cudaStreamCreateWithPriority for live manual overlay",
                "cudaEventCreateWithFlags for live manual overlay");
        } catch (...) {
            destroy_resources();
            throw;
        }
    }

    void destroy_resources() noexcept {
        (void)cudaSetDevice(cuda_device_index);
        overlay_slots.clear();
    }

    void worker_thread_main() {
        std::uint64_t last_generation = 0;
        worker_runtime::run_worker_thread_main(
            running, stop_requested, status, cuda_device_index, "cudaSetDevice for live manual overlay thread",
            "live.manual_overlay",
            [this, &last_generation](Status& current_status) {
                const std::shared_ptr<const ManualOverlayDocumentSnapshot> snapshot =
                    document.wait_snapshot_if_changed(last_generation, stop_requested);
                if (!snapshot) {
                    return;
                }

                render_snapshot(*snapshot);
                last_generation = snapshot->generation;
                current_status.generations_rendered += 1U;
                current_status.last_generation = last_generation;
                current_status.last_error.clear();
                worker_runtime::publish_status(&status, current_status);
            },
            [this](const char* error_message) { publish_error(error_message); });
    }

    static void add_packed_count(const std::size_t count, std::size_t* total, const char* context) {
        if (total == nullptr || count > std::numeric_limits<std::size_t>::max() - *total) {
            throw std::overflow_error(context);
        }
        *total += count;
    }

    static void prepare_snapshot_uploads(const ManualOverlayDocumentSnapshot& snapshot, OverlaySlot* slot,
                                         const OverlayTarget& target) {
        if (snapshot.interaction_instances.size() >
            std::numeric_limits<std::size_t>::max() - snapshot.instances.size()) {
            throw std::overflow_error("manual overlay instance count overflow");
        }
        const std::size_t instance_count = snapshot.instances.size() + snapshot.interaction_instances.size();
        slot->packed_instances.resize(instance_count);
        const auto instance_at = [&snapshot](const std::size_t index) -> const ManualOverlayInstance& {
            return index < snapshot.instances.size()
                       ? snapshot.instances[index]
                       : snapshot.interaction_instances[index - snapshot.instances.size()];
        };
        std::size_t mask_bytes_total = 0U;
        std::size_t mask_run_values_total = 0U;
        std::size_t point_values_total = 0U;
        std::size_t edge_values_total = 0U;

        for (std::size_t index = 0U; index < instance_count; ++index) {
            const ManualOverlayInstance& instance = instance_at(index);
            PackedInstance& packed = slot->packed_instances[index];
            packed.mask_offset = mask_bytes_total;
            packed.run_value_offset = mask_run_values_total;
            packed.polyline_value_offset = point_values_total;
            packed.point_value_offset = point_values_total;
            packed.edge_value_offset = edge_values_total;
            if (!instance.enabled) {
                continue;
            }

            const std::size_t mask_bytes = static_cast<std::size_t>(instance.mask_region.width) *
                                           static_cast<std::size_t>(instance.mask_region.height);
            if (mask_bytes > 0U && instance.mask.size() == mask_bytes) {
                add_packed_count(mask_bytes, &mask_bytes_total, "manual overlay packed mask size overflow");
            } else if (mask_bytes > 0U && !instance.mask_runs.empty()) {
                if (instance.mask_runs.size() > std::numeric_limits<std::size_t>::max() / 2U) {
                    throw std::overflow_error("manual overlay packed mask-run size overflow");
                }
                add_packed_count(instance.mask_runs.size() * 2U, &mask_run_values_total,
                                 "manual overlay packed mask-run size overflow");
            }
            if (instance.polyline_points.size() > std::numeric_limits<std::size_t>::max() / 2U ||
                instance.points.size() > std::numeric_limits<std::size_t>::max() / 2U ||
                instance.skeleton_edges.size() > std::numeric_limits<std::size_t>::max() / 2U) {
                throw std::overflow_error("manual overlay packed geometry size overflow");
            }
            packed.polyline_value_offset = point_values_total;
            add_packed_count(instance.polyline_points.size() * 2U, &point_values_total,
                             "manual overlay packed point size overflow");
            packed.point_value_offset = point_values_total;
            add_packed_count(instance.points.size() * 2U, &point_values_total,
                             "manual overlay packed point size overflow");
            packed.edge_value_offset = edge_values_total;
            add_packed_count(instance.skeleton_edges.size() * 2U, &edge_values_total,
                             "manual overlay packed edge size overflow");
        }

        if (mask_bytes_total > 0U) {
            ensure_pinned_upload_capacity<std::uint8_t>(mask_bytes_total, &slot->host_mask_upload,
                                                        "cudaHostAlloc for packed manual overlay masks");
            ensure_device_upload_capacity<std::uint8_t>(mask_bytes_total, &slot->device_mask_upload,
                                                        "cudaMalloc for packed manual overlay masks");
        }
        if (mask_run_values_total > 0U) {
            ensure_pinned_upload_capacity<std::uint32_t>(mask_run_values_total, &slot->host_mask_runs_upload,
                                                         "cudaHostAlloc for packed manual overlay mask runs");
            ensure_device_upload_capacity<std::uint32_t>(mask_run_values_total, &slot->device_mask_runs_upload,
                                                         "cudaMalloc for packed manual overlay mask runs");
        }
        if (point_values_total > 0U) {
            ensure_pinned_upload_capacity<int>(point_values_total, &slot->host_points_upload,
                                               "cudaHostAlloc for packed manual overlay points");
            ensure_device_upload_capacity<int>(point_values_total, &slot->device_points_upload,
                                               "cudaMalloc for packed manual overlay points");
        }
        if (edge_values_total > 0U) {
            ensure_pinned_upload_capacity<std::uint32_t>(edge_values_total, &slot->host_edges_upload,
                                                         "cudaHostAlloc for packed manual overlay edges");
            ensure_device_upload_capacity<std::uint32_t>(edge_values_total, &slot->device_edges_upload,
                                                         "cudaMalloc for packed manual overlay edges");
        }

        for (std::size_t index = 0U; index < instance_count; ++index) {
            const ManualOverlayInstance& instance = instance_at(index);
            const PackedInstance& packed = slot->packed_instances[index];
            if (!instance.enabled) {
                continue;
            }
            const std::size_t mask_bytes = static_cast<std::size_t>(instance.mask_region.width) *
                                           static_cast<std::size_t>(instance.mask_region.height);
            if (mask_bytes > 0U && instance.mask.size() == mask_bytes) {
                std::memcpy(slot->host_mask_upload.data() + packed.mask_offset, instance.mask.data(), mask_bytes);
            } else if (mask_bytes > 0U && !instance.mask_runs.empty()) {
                std::uint32_t* runs = slot->host_mask_runs_upload.data() + packed.run_value_offset;
                for (const ManualOverlayMaskRun& run : instance.mask_runs) {
                    *runs++ = run.offset;
                    *runs++ = run.length;
                }
            }
            if (!instance.polyline_points.empty()) {
                (void)pack_points_xy(instance.polyline_points,
                                     slot->host_points_upload.data() + packed.polyline_value_offset);
            }
            if (!instance.points.empty()) {
                (void)pack_points_xy(instance.points, slot->host_points_upload.data() + packed.point_value_offset);
            }
            if (!instance.skeleton_edges.empty()) {
                (void)pack_edge_indices(instance.skeleton_edges,
                                        slot->host_edges_upload.data() + packed.edge_value_offset);
            }
        }

        if (mask_bytes_total > 0U) {
            ensure_cuda_ok(
                cudaMemcpyAsync(device_ptr_as_void(slot->device_mask_upload.data()), slot->host_mask_upload.data(),
                                mask_bytes_total, cudaMemcpyHostToDevice, target.stream),
                "cudaMemcpyAsync for packed manual overlay masks");
        }
        if (mask_run_values_total > 0U) {
            ensure_cuda_ok(
                cudaMemcpyAsync(device_ptr_as_void(slot->device_mask_runs_upload.data()),
                                slot->host_mask_runs_upload.data(), mask_run_values_total * sizeof(std::uint32_t),
                                cudaMemcpyHostToDevice, target.stream),
                "cudaMemcpyAsync for packed manual overlay mask runs");
        }
        if (point_values_total > 0U) {
            ensure_cuda_ok(
                cudaMemcpyAsync(device_ptr_as_void(slot->device_points_upload.data()), slot->host_points_upload.data(),
                                point_values_total * sizeof(int), cudaMemcpyHostToDevice, target.stream),
                "cudaMemcpyAsync for packed manual overlay points");
        }
        if (edge_values_total > 0U) {
            ensure_cuda_ok(
                cudaMemcpyAsync(device_ptr_as_void(slot->device_edges_upload.data()), slot->host_edges_upload.data(),
                                edge_values_total * sizeof(std::uint32_t), cudaMemcpyHostToDevice, target.stream),
                "cudaMemcpyAsync for packed manual overlay edges");
        }
    }

    void render_snapshot(const ManualOverlayDocumentSnapshot& snapshot) {
        OverlaySlot* slot = reserve_overlay_slot();
        if (slot == nullptr) {
            return;
        }

        try {
            const OverlayTarget target{
                device_ptr_as_bytes(slot->device_buffer.data()),
                slot->device_buffer.pitch_bytes(),
                static_cast<int>(slot->device_buffer.width()),
                static_cast<int>(slot->device_buffer.height()),
                slot->stream.get(),
            };

            const std::size_t full_width_bytes = static_cast<std::size_t>(slot->device_buffer.width()) * 4U;
            ensure_cuda_ok(cudaMemset2DAsync(device_ptr_as_void(slot->device_buffer.data()), target.pitch, 0,
                                             full_width_bytes, slot->device_buffer.height(), target.stream),
                           "cudaMemset2DAsync for live manual overlay clear");
            prepare_snapshot_uploads(snapshot, slot, target);

            bool has_content = false;
            const std::size_t instance_count = snapshot.instances.size() + snapshot.interaction_instances.size();
            for (std::size_t instance_index = 0U; instance_index < instance_count; ++instance_index) {
                const ManualOverlayInstance& instance =
                    instance_index < snapshot.instances.size()
                        ? snapshot.instances[instance_index]
                        : snapshot.interaction_instances[instance_index - snapshot.instances.size()];
                const PackedInstance& packed = slot->packed_instances[instance_index];
                if (!instance.enabled) {
                    continue;
                }

                const std::array<std::uint8_t, 3> color =
                    instance.style.has_value()
                        ? std::array<std::uint8_t, 3>{instance.style->r, instance.style->g, instance.style->b}
                        : category_color(instance.category_index);
                const std::uint8_t mask_alpha = instance.style.has_value() ? instance.style->alpha : 97U;
                const std::uint8_t point_alpha = instance.style.has_value() ? instance.style->alpha : 240U;
                const int line_thickness = std::max(1, instance.style.has_value() ? instance.style->line_thickness : 2);
                const int point_radius = std::max(1, instance.style.has_value() ? instance.style->point_radius : 3);
                const std::size_t mask_bytes = static_cast<std::size_t>(instance.mask_region.width) *
                                               static_cast<std::size_t>(instance.mask_region.height);
                if (mask_bytes > 0U && instance.mask.size() == mask_bytes &&
                    instance.mask_region.capture_x < slot->device_buffer.width() &&
                    instance.mask_region.capture_y < slot->device_buffer.height()) {
                    draw_manual_mask(target, instance, color, mask_alpha,
                                     slot->device_mask_upload.data() + packed.mask_offset);
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_manual_mask_rgba_pitched");
                    has_content = true;
                } else if (mask_bytes > 0U && !instance.mask_runs.empty() &&
                           instance.mask_region.capture_x < slot->device_buffer.width() &&
                           instance.mask_region.capture_y < slot->device_buffer.height()) {
                    draw_manual_mask_runs(
                        target, instance, color, mask_alpha,
                        slot->device_mask_runs_upload.data() + packed.run_value_offset * sizeof(std::uint32_t));
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_manual_mask_runs_rgba_pitched");
                    has_content = true;
                }

                if (instance.polyline_points.size() >= 2U) {
                    draw_polyline(target, instance, color, line_thickness,
                                  slot->device_points_upload.data() + packed.polyline_value_offset * sizeof(int));
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_polyline_rgba_pitched");
                    has_content = true;
                }

                if (!instance.skeleton_edges.empty() && !instance.points.empty()) {
                    draw_skeleton(target, instance, color, line_thickness,
                                  slot->device_points_upload.data() + packed.point_value_offset * sizeof(int),
                                  slot->device_edges_upload.data() + packed.edge_value_offset * sizeof(std::uint32_t));
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_skeleton_rgba_pitched");
                    has_content = true;
                }

                if (!instance.points.empty()) {
                    draw_points(target, instance, color, point_radius, point_alpha,
                                slot->device_points_upload.data() + packed.point_value_offset * sizeof(int));
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_points_rgba_pitched");
                    has_content = true;
                }

                if (box_has_area(instance.box)) {
                    draw_box_outline(target, instance, color, line_thickness);
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_box_outline_rgba_pitched");
                    has_content = true;
                    if (instance.style.has_value() && instance.style->draw_handles) {
                        draw_selection_handles(target, instance, color, std::max(1, instance.style->handle_radius),
                                               instance.style->alpha);
                        ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_selection_handles_rgba_pitched styled");
                    }
                }
            }

            if (snapshot.selected_instance.has_value() && *snapshot.selected_instance < snapshot.instances.size()) {
                const ManualOverlayInstance& selected = snapshot.instances[*snapshot.selected_instance];
                if (box_has_area(selected.box)) {
                    draw_selection_handles(target, selected, {255U, 220U, 96U}, 4, 240U);
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_selection_handles_rgba_pitched");
                    has_content = true;
                }
            }
            if (snapshot.brush_preview.has_value()) {
                draw_brush_preview(target, *snapshot.brush_preview, slot->host_brush_points_upload,
                                   slot->device_brush_points_upload);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_polyline_rgba_pitched for brush preview");
                has_content = true;
            }

            ensure_cuda_ok(cudaEventRecord(slot->ready_event.get(), target.stream),
                           "cudaEventRecord for live manual overlay");
            slot->generation = snapshot.generation;
            slot->has_content = has_content;
            worker_runtime::publish_latest_slot(*slot, latest_overlay_index);
        } catch (...) {
            if (cudaEventRecord(slot->ready_event.get(), slot->stream.get()) == cudaSuccess) {
                slot->state.store(to_slot_state_value(SlotState::kPendingFree), std::memory_order_release);
            } else {
                (void)cudaStreamSynchronize(slot->stream.get());
                worker_runtime::release_writing_slot(*slot);
            }
            throw;
        }
    }

    [[nodiscard]] OverlaySlot* reserve_overlay_slot() {
        return worker_runtime::reserve_writable_slot_view(overlay_slots, latest_overlay_index,
                                                          "cudaEventQuery for live manual overlay slot reuse");
    }

    void publish_error(std::string error_message) {
        worker_runtime::publish_error(&last_error, std::move(error_message));
    }

    ManualOverlayDocument& document;
    int cuda_device_index = 0;
    std::uint32_t max_capture_width = 0;
    std::uint32_t max_capture_height = 0;
    std::thread thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<std::shared_ptr<const Status>> status{std::make_shared<Status>()};
    std::atomic<std::shared_ptr<const std::string>> last_error{std::make_shared<std::string>()};
    std::vector<std::unique_ptr<OverlaySlot>> overlay_slots;
    std::atomic<int> latest_overlay_index{-1};
};

LiveManualOverlayWorker::LiveManualOverlayWorker(ManualOverlayDocument& document, const int cuda_device_index,
                                                 const std::uint32_t max_capture_width,
                                                 const std::uint32_t max_capture_height,
                                                 const std::uint32_t overlay_slot_count)
    : impl_(std::make_unique<Impl>(document, cuda_device_index, max_capture_width, max_capture_height,
                                   overlay_slot_count)) {}

LiveManualOverlayWorker::~LiveManualOverlayWorker() = default;

void LiveManualOverlayWorker::start() {
    impl_->start();
}

void LiveManualOverlayWorker::stop() {
    impl_->stop();
}

bool LiveManualOverlayWorker::running() const noexcept {
    return impl_->running_state();
}

bool LiveManualOverlayWorker::try_acquire_latest_overlay(OverlayView* out) {
    return impl_->try_acquire_latest_overlay(out);
}

void LiveManualOverlayWorker::release_overlay(const std::uint32_t slot_index) {
    impl_->release_overlay(slot_index);
}

LiveManualOverlayWorker::Status LiveManualOverlayWorker::snapshot_status() const {
    return impl_->snapshot_status();
}

}  
