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
                      const std::array<std::uint8_t, 3>& color, const DeviceUploadBuffer<std::uint8_t>& mask_upload) {
    std::uint8_t* overlay_region = target.data +
                                   static_cast<std::size_t>(instance.mask_region.capture_y) * target.pitch +
                                   static_cast<std::size_t>(instance.mask_region.capture_x) * 4U;
    mmltk::rfdetr::launch_draw_manual_mask_rgba_pitched(
        overlay_region, target.pitch, static_cast<int>(instance.mask_region.width),
        static_cast<int>(instance.mask_region.height), device_ptr_as_bytes(mask_upload.data()), color[0], color[1],
        color[2], 97U, target.stream);
}

void draw_polyline(const OverlayTarget& target, const ManualOverlayInstance& instance,
                   const std::array<std::uint8_t, 3>& color, const DeviceUploadBuffer<int>& points_upload) {
    mmltk::rfdetr::launch_draw_polyline_rgba_pitched(
        target.data, target.pitch, target.width, target.height,
        reinterpret_cast<const int*>(device_ptr_as_void(points_upload.data())),
        static_cast<int>(instance.polyline_points.size()), instance.polyline_closed, color[0], color[1], color[2], 2,
        target.stream);
}

void draw_skeleton(const OverlayTarget& target, const ManualOverlayInstance& instance,
                   const std::array<std::uint8_t, 3>& color, const DeviceUploadBuffer<int>& points_upload,
                   const DeviceUploadBuffer<std::uint32_t>& edges_upload) {
    mmltk::rfdetr::launch_draw_skeleton_rgba_pitched(
        target.data, target.pitch, target.width, target.height,
        reinterpret_cast<const int*>(device_ptr_as_void(points_upload.data())),
        static_cast<int>(instance.points.size()),
        reinterpret_cast<const std::uint32_t*>(device_ptr_as_void(edges_upload.data())),
        static_cast<int>(instance.skeleton_edges.size()), color[0], color[1], color[2], 2, target.stream);
}

void draw_points(const OverlayTarget& target, const ManualOverlayInstance& instance,
                 const std::array<std::uint8_t, 3>& color, const DeviceUploadBuffer<int>& points_upload) {
    mmltk::rfdetr::launch_draw_points_rgba_pitched(
        target.data, target.pitch, target.width, target.height,
        reinterpret_cast<const int*>(device_ptr_as_void(points_upload.data())),
        static_cast<int>(instance.points.size()), 3, color[0], color[1], color[2], 240U, target.stream);
}

void draw_box_outline(const OverlayTarget& target, const ManualOverlayInstance& instance,
                      const std::array<std::uint8_t, 3>& color) {
    mmltk::rfdetr::launch_draw_box_outline_rgba_pitched(
        target.data, target.pitch, target.width, target.height, instance.box.x1, instance.box.y1, instance.box.x2,
        instance.box.y2, color[0], color[1], color[2], 2, target.stream);
}

void draw_selection_handles(const OverlayTarget& target, const ManualOverlayInstance& selected) {
    mmltk::rfdetr::launch_draw_selection_handles_rgba_pitched(target.data, target.pitch, target.width, target.height,
                                                              selected.box.x1, selected.box.y1, selected.box.x2,
                                                              selected.box.y2, 4, target.stream);
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
        points_xy[index * 2] = static_cast<int>(std::lround(static_cast<double>(brush_preview.capture_x) +
                                                            std::cos(theta) * radius));
        points_xy[index * 2 + 1] = static_cast<int>(std::lround(static_cast<double>(brush_preview.capture_y) +
                                                                std::sin(theta) * radius));
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

}  // namespace

struct LiveManualOverlayWorker::Impl {
    struct OverlaySlot {
        std::uint32_t slot_index = 0;
        RgbaPitchedDeviceBuffer device_buffer;
        CudaStreamHandle stream;
        CudaEventHandle ready_event;
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
        device_edges_upload.reset();
        host_edges_upload.reset();
        device_points_upload.reset();
        host_points_upload.reset();
        device_mask_upload.reset();
        host_mask_upload.reset();
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

            bool has_content = false;
            for (const ManualOverlayInstance& instance : snapshot.instances) {
                if (!instance.enabled) {
                    continue;
                }

                const std::array<std::uint8_t, 3> color = category_color(instance.category_index);
                bool instance_points_uploaded = false;
                const std::size_t mask_bytes = static_cast<std::size_t>(instance.mask_region.width) *
                                               static_cast<std::size_t>(instance.mask_region.height);
                if (mask_bytes > 0U && instance.mask.size() == mask_bytes &&
                    instance.mask_region.capture_x < slot->device_buffer.width() &&
                    instance.mask_region.capture_y < slot->device_buffer.height()) {
                    ensure_pinned_upload_capacity<std::uint8_t>(mask_bytes, &host_mask_upload,
                                                                "cudaHostAlloc for manual overlay mask upload");
                    ensure_device_upload_capacity<std::uint8_t>(mask_bytes, &device_mask_upload,
                                                                "cudaMalloc for manual overlay mask upload");
                    std::memcpy(host_mask_upload.data(), instance.mask.data(), mask_bytes);
                    ensure_cuda_ok(
                        cudaMemcpyAsync(device_ptr_as_void(device_mask_upload.data()), host_mask_upload.data(),
                                        mask_bytes, cudaMemcpyHostToDevice, target.stream),
                        "cudaMemcpyAsync for manual overlay mask upload");

                    draw_manual_mask(target, instance, color, device_mask_upload);
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_manual_mask_rgba_pitched");
                    has_content = true;
                }

                if (instance.polyline_points.size() >= 2U) {
                    const std::size_t point_value_count = instance.polyline_points.size() * 2U;
                    ensure_pinned_upload_capacity<int>(point_value_count, &host_points_upload,
                                                       "cudaHostAlloc for manual overlay point upload");
                    ensure_device_upload_capacity<int>(point_value_count, &device_points_upload,
                                                       "cudaMalloc for manual overlay point upload");
                    const std::size_t packed_value_count =
                        pack_points_xy(instance.polyline_points, host_points_upload.data());
                    ensure_cuda_ok(
                        cudaMemcpyAsync(device_ptr_as_void(device_points_upload.data()), host_points_upload.data(),
                                        packed_value_count * sizeof(int), cudaMemcpyHostToDevice, target.stream),
                        "cudaMemcpyAsync for manual overlay polyline upload");
                    draw_polyline(target, instance, color, device_points_upload);
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_polyline_rgba_pitched");
                    has_content = true;
                }

                if (!instance.skeleton_edges.empty() && !instance.points.empty()) {
                    const std::size_t point_value_count = instance.points.size() * 2U;
                    const std::size_t edge_value_count = instance.skeleton_edges.size() * 2U;
                    ensure_pinned_upload_capacity<int>(point_value_count, &host_points_upload,
                                                       "cudaHostAlloc for manual overlay skeleton point upload");
                    ensure_device_upload_capacity<int>(point_value_count, &device_points_upload,
                                                       "cudaMalloc for manual overlay skeleton point upload");
                    ensure_pinned_upload_capacity<std::uint32_t>(
                        edge_value_count, &host_edges_upload, "cudaHostAlloc for manual overlay skeleton edge upload");
                    ensure_device_upload_capacity<std::uint32_t>(edge_value_count, &device_edges_upload,
                                                                 "cudaMalloc for manual overlay skeleton edge upload");
                    const std::size_t packed_point_values = pack_points_xy(instance.points, host_points_upload.data());
                    const std::size_t packed_edge_values =
                        pack_edge_indices(instance.skeleton_edges, host_edges_upload.data());
                    ensure_cuda_ok(
                        cudaMemcpyAsync(device_ptr_as_void(device_points_upload.data()), host_points_upload.data(),
                                        packed_point_values * sizeof(int), cudaMemcpyHostToDevice, target.stream),
                        "cudaMemcpyAsync for manual overlay skeleton point upload");
                    instance_points_uploaded = true;
                    ensure_cuda_ok(cudaMemcpyAsync(device_ptr_as_void(device_edges_upload.data()),
                                                   host_edges_upload.data(), packed_edge_values * sizeof(std::uint32_t),
                                                   cudaMemcpyHostToDevice, target.stream),
                                   "cudaMemcpyAsync for manual overlay skeleton edge upload");
                    draw_skeleton(target, instance, color, device_points_upload, device_edges_upload);
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_skeleton_rgba_pitched");
                    has_content = true;
                }

                if (!instance.points.empty()) {
                    const std::size_t point_value_count = instance.points.size() * 2U;
                    if (!instance_points_uploaded) {
                        ensure_pinned_upload_capacity<int>(point_value_count, &host_points_upload,
                                                           "cudaHostAlloc for manual overlay marker upload");
                        ensure_device_upload_capacity<int>(point_value_count, &device_points_upload,
                                                           "cudaMalloc for manual overlay marker upload");
                        const std::size_t packed_value_count =
                            pack_points_xy(instance.points, host_points_upload.data());
                        ensure_cuda_ok(
                            cudaMemcpyAsync(device_ptr_as_void(device_points_upload.data()), host_points_upload.data(),
                                            packed_value_count * sizeof(int), cudaMemcpyHostToDevice, target.stream),
                            "cudaMemcpyAsync for manual overlay point marker upload");
                    }
                    draw_points(target, instance, color, device_points_upload);
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_points_rgba_pitched");
                    has_content = true;
                }

                if (box_has_area(instance.box)) {
                    draw_box_outline(target, instance, color);
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_box_outline_rgba_pitched");
                    has_content = true;
                }
            }

            if (snapshot.selected_instance.has_value() && *snapshot.selected_instance < snapshot.instances.size()) {
                const ManualOverlayInstance& selected = snapshot.instances[*snapshot.selected_instance];
                if (box_has_area(selected.box)) {
                    draw_selection_handles(target, selected);
                    ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_selection_handles_rgba_pitched");
                    has_content = true;
                }
            }
            if (snapshot.brush_preview.has_value()) {
                draw_brush_preview(target, *snapshot.brush_preview, host_points_upload, device_points_upload);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_polyline_rgba_pitched for brush preview");
                has_content = true;
            }

            ensure_cuda_ok(cudaEventRecord(slot->ready_event.get(), target.stream),
                           "cudaEventRecord for live manual overlay");
            slot->generation = snapshot.generation;
            slot->has_content = has_content;
            worker_runtime::publish_latest_slot(*slot, latest_overlay_index);
        } catch (...) {
            worker_runtime::release_writing_slot(*slot);
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
    PinnedUploadBuffer<std::uint8_t> host_mask_upload;
    DeviceUploadBuffer<std::uint8_t> device_mask_upload;
    PinnedUploadBuffer<int> host_points_upload;
    DeviceUploadBuffer<int> device_points_upload;
    PinnedUploadBuffer<std::uint32_t> host_edges_upload;
    DeviceUploadBuffer<std::uint32_t> device_edges_upload;
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

}  // namespace mmltk::live
