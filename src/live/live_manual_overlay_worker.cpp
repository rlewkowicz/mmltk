#include "mmltk/live/live_manual_overlay_worker.h"

#include "live/upload_buffer_utils.h"
#include "mmltk/live/live_worker_runtime.h"
#include "mmltk/rfdetr/draw_cuda.h"
#include "mmltk_logging.h"

#include <algorithm>
#include <array>
#include <chrono>
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

void draw_manual_mask(const OverlayTarget& target,
                      const ManualOverlayInstance& instance,
                      const std::array<std::uint8_t, 3>& color,
                      const DeviceUploadBuffer<std::uint8_t>& mask_upload) {
    std::uint8_t* overlay_region =
        target.data +
        static_cast<std::size_t>(instance.mask_region.capture_y) * target.pitch +
        static_cast<std::size_t>(instance.mask_region.capture_x) * 4U;
    mmltk::rfdetr::launch_draw_manual_mask_rgba_pitched(
        overlay_region,
        target.pitch,
        static_cast<int>(instance.mask_region.width),
        static_cast<int>(instance.mask_region.height),
        device_ptr_as_bytes(mask_upload.data()),
        color[0],
        color[1],
        color[2],
        97U,
        target.stream);
}

void draw_polyline(const OverlayTarget& target,
                   const ManualOverlayInstance& instance,
                   const std::array<std::uint8_t, 3>& color,
                   const DeviceUploadBuffer<int>& points_upload) {
    mmltk::rfdetr::launch_draw_polyline_rgba_pitched(
        target.data,
        target.pitch,
        target.width,
        target.height,
        reinterpret_cast<const int*>(device_ptr_as_void(points_upload.data())),
        static_cast<int>(instance.polyline_points.size()),
        instance.polyline_closed,
        color[0],
        color[1],
        color[2],
        2,
        target.stream);
}

void draw_skeleton(const OverlayTarget& target,
                   const ManualOverlayInstance& instance,
                   const std::array<std::uint8_t, 3>& color,
                   const DeviceUploadBuffer<int>& points_upload,
                   const DeviceUploadBuffer<std::uint32_t>& edges_upload) {
    mmltk::rfdetr::launch_draw_skeleton_rgba_pitched(
        target.data,
        target.pitch,
        target.width,
        target.height,
        reinterpret_cast<const int*>(device_ptr_as_void(points_upload.data())),
        static_cast<int>(instance.points.size()),
        reinterpret_cast<const std::uint32_t*>(device_ptr_as_void(edges_upload.data())),
        static_cast<int>(instance.skeleton_edges.size()),
        color[0],
        color[1],
        color[2],
        2,
        target.stream);
}

void draw_points(const OverlayTarget& target,
                 const ManualOverlayInstance& instance,
                 const std::array<std::uint8_t, 3>& color,
                 const DeviceUploadBuffer<int>& points_upload) {
    mmltk::rfdetr::launch_draw_points_rgba_pitched(
        target.data,
        target.pitch,
        target.width,
        target.height,
        reinterpret_cast<const int*>(device_ptr_as_void(points_upload.data())),
        static_cast<int>(instance.points.size()),
        3,
        color[0],
        color[1],
        color[2],
        240U,
        target.stream);
}

void draw_box_outline(const OverlayTarget& target,
                      const ManualOverlayInstance& instance,
                      const std::array<std::uint8_t, 3>& color) {
    mmltk::rfdetr::launch_draw_box_outline_rgba_pitched(
        target.data,
        target.pitch,
        target.width,
        target.height,
        instance.box.x1,
        instance.box.y1,
        instance.box.x2,
        instance.box.y2,
        color[0],
        color[1],
        color[2],
        2,
        target.stream);
}

void draw_selection_handles(const OverlayTarget& target,
                            const ManualOverlayInstance& selected) {
    mmltk::rfdetr::launch_draw_selection_handles_rgba_pitched(
        target.data,
        target.pitch,
        target.width,
        target.height,
        selected.box.x1,
        selected.box.y1,
        selected.box.x2,
        selected.box.y2,
        4,
        target.stream);
}

} // namespace

LiveManualOverlayWorker::LiveManualOverlayWorker(ManualOverlayDocument& document,
                                                 const int cuda_device_index,
                                                 const std::uint32_t max_capture_width,
                                                 const std::uint32_t max_capture_height,
                                                 const std::uint32_t overlay_slot_count)
    : document_(document),
      cuda_device_index_(cuda_device_index),
      max_capture_width_(std::max<std::uint32_t>(1U, max_capture_width)),
      max_capture_height_(std::max<std::uint32_t>(1U, max_capture_height)) {
    if (overlay_slot_count == 0U) {
        throw std::runtime_error("live manual overlay worker requires at least one overlay slot");
    }
    overlay_slots_.reserve(overlay_slot_count);
    allocate_resources();
}

LiveManualOverlayWorker::~LiveManualOverlayWorker() {
    try {
        stop();
    } catch (const std::exception& error) {
        log_live_manual_overlay_message("error", error.what());
    }
    destroy_resources();
}

void LiveManualOverlayWorker::start() {
    if (running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("live manual overlay worker is already running");
    }
    if (thread_.joinable()) {
        throw std::runtime_error("live manual overlay worker must be stopped before restart");
    }

    publish_error({});
    worker_runtime::publish_status(&status_, Status{});
    ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for live manual overlay start");
    worker_runtime::reset_slot_views_for_restart(overlay_slots_,
                                                 latest_overlay_index_,
                                                 "cudaEventSynchronize for live manual overlay slot restart");
    worker_runtime::start_worker_thread(
        thread_,
        stop_requested_,
        running_,
        [this]() { worker_thread_main(); },
        "live manual overlay worker is already running",
        "live manual overlay worker must be stopped before restart");
}

void LiveManualOverlayWorker::stop() {
    worker_runtime::stop_worker_thread(thread_, stop_requested_, running_);
}

bool LiveManualOverlayWorker::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

bool LiveManualOverlayWorker::try_acquire_latest_overlay(OverlayView* out) {
    if (out == nullptr) {
        throw std::runtime_error("live manual overlay worker requires an overlay output");
    }
    *out = {};
    if (!running()) {
        return false;
    }
    return worker_runtime::try_acquire_latest_published_slot_view(overlay_slots_,
                                                                   latest_overlay_index_,
                                                                   out);
}

void LiveManualOverlayWorker::release_overlay(const std::uint32_t slot_index) {
    worker_runtime::release_slot_by_index(
        overlay_slots_,
        slot_index,
        "live manual overlay worker release index out of range",
        "live manual overlay worker release called for a slot that is not acquired");
}

LiveManualOverlayWorker::Status LiveManualOverlayWorker::snapshot_status() const {
    return worker_runtime::snapshot_status(status_);
}

void LiveManualOverlayWorker::allocate_resources() {
    ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for live manual overlay resource allocation");

    try {
        worker_runtime::allocate_pitched_device_slots(
            overlay_slots_,
            static_cast<std::uint32_t>(overlay_slots_.capacity()),
            max_capture_width_,
            max_capture_height_,
            "cudaMallocPitch for live manual overlay",
            "cudaStreamCreateWithPriority for live manual overlay",
            "cudaEventCreateWithFlags for live manual overlay");
    } catch (...) {
        destroy_resources();
        throw;
    }
}

void LiveManualOverlayWorker::destroy_resources() noexcept {
    (void)cudaSetDevice(cuda_device_index_);
    device_edges_upload_.reset();
    host_edges_upload_.reset();
    device_points_upload_.reset();
    host_points_upload_.reset();
    device_mask_upload_.reset();
    host_mask_upload_.reset();
    overlay_slots_.clear();
}

void LiveManualOverlayWorker::worker_thread_main() {
    std::uint64_t last_generation = 0;
    worker_runtime::run_worker_thread_main(
        running_,
        stop_requested_,
        status_,
        cuda_device_index_,
        "cudaSetDevice for live manual overlay thread",
        "live.manual_overlay",
        [this, &last_generation](Status& status) {
            const std::shared_ptr<const ManualOverlayDocumentSnapshot> snapshot =
                document_.snapshot_if_changed(last_generation);
            if (!snapshot) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return;
            }

            render_snapshot(*snapshot);
            last_generation = snapshot->generation;
            status.generations_rendered += 1U;
            status.last_generation = last_generation;
            status.last_error.clear();
            worker_runtime::publish_status(&status_, status);
        },
        [this](const char* error_message) {
            publish_error(error_message);
        });
}

void LiveManualOverlayWorker::render_snapshot(const ManualOverlayDocumentSnapshot& snapshot) {
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
        ensure_cuda_ok(cudaMemset2DAsync(device_ptr_as_void(slot->device_buffer.data()),
                                         target.pitch,
                                         0,
                                         full_width_bytes,
                                         slot->device_buffer.height(),
                                         target.stream),
                       "cudaMemset2DAsync for live manual overlay clear");

        bool has_content = false;
        for (const ManualOverlayInstance& instance : snapshot.instances) {
            if (!instance.enabled) {
                continue;
            }

            const std::array<std::uint8_t, 3> color = category_color(instance.category_index);
            bool instance_points_uploaded = false;
            const std::size_t mask_bytes =
                static_cast<std::size_t>(instance.mask_region.width) * static_cast<std::size_t>(instance.mask_region.height);
            if (mask_bytes > 0U &&
                instance.mask.size() == mask_bytes &&
                instance.mask_region.capture_x < slot->device_buffer.width() &&
                instance.mask_region.capture_y < slot->device_buffer.height()) {
                ensure_pinned_upload_capacity<std::uint8_t>(
                    mask_bytes,
                    &host_mask_upload_,
                    "cudaHostAlloc for manual overlay mask upload");
                ensure_device_upload_capacity<std::uint8_t>(
                    mask_bytes,
                    &device_mask_upload_,
                    "cudaMalloc for manual overlay mask upload");
                std::memcpy(host_mask_upload_.data(), instance.mask.data(), mask_bytes);
                ensure_cuda_ok(cudaMemcpyAsync(device_ptr_as_void(device_mask_upload_.data()),
                                               host_mask_upload_.data(),
                                               mask_bytes,
                                               cudaMemcpyHostToDevice,
                                               target.stream),
                               "cudaMemcpyAsync for manual overlay mask upload");

                draw_manual_mask(target, instance, color, device_mask_upload_);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_manual_mask_rgba_pitched");
                has_content = true;
            }

            if (instance.polyline_points.size() >= 2U) {
                const std::size_t point_value_count =
                    instance.polyline_points.size() * 2U;
                ensure_pinned_upload_capacity<int>(
                    point_value_count,
                    &host_points_upload_,
                    "cudaHostAlloc for manual overlay point upload");
                ensure_device_upload_capacity<int>(
                    point_value_count,
                    &device_points_upload_,
                    "cudaMalloc for manual overlay point upload");
                const std::size_t packed_value_count =
                    pack_points_xy(instance.polyline_points, host_points_upload_.data());
                ensure_cuda_ok(cudaMemcpyAsync(device_ptr_as_void(device_points_upload_.data()),
                                               host_points_upload_.data(),
                                               packed_value_count * sizeof(int),
                                               cudaMemcpyHostToDevice,
                                               target.stream),
                               "cudaMemcpyAsync for manual overlay polyline upload");
                draw_polyline(target, instance, color, device_points_upload_);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_polyline_rgba_pitched");
                has_content = true;
            }

            if (!instance.skeleton_edges.empty() && !instance.points.empty()) {
                const std::size_t point_value_count = instance.points.size() * 2U;
                const std::size_t edge_value_count = instance.skeleton_edges.size() * 2U;
                ensure_pinned_upload_capacity<int>(
                    point_value_count,
                    &host_points_upload_,
                    "cudaHostAlloc for manual overlay skeleton point upload");
                ensure_device_upload_capacity<int>(
                    point_value_count,
                    &device_points_upload_,
                    "cudaMalloc for manual overlay skeleton point upload");
                ensure_pinned_upload_capacity<std::uint32_t>(
                    edge_value_count,
                    &host_edges_upload_,
                    "cudaHostAlloc for manual overlay skeleton edge upload");
                ensure_device_upload_capacity<std::uint32_t>(
                    edge_value_count,
                    &device_edges_upload_,
                    "cudaMalloc for manual overlay skeleton edge upload");
                const std::size_t packed_point_values =
                    pack_points_xy(instance.points, host_points_upload_.data());
                const std::size_t packed_edge_values =
                    pack_edge_indices(instance.skeleton_edges, host_edges_upload_.data());
                ensure_cuda_ok(cudaMemcpyAsync(device_ptr_as_void(device_points_upload_.data()),
                                               host_points_upload_.data(),
                                               packed_point_values * sizeof(int),
                                               cudaMemcpyHostToDevice,
                                               target.stream),
                               "cudaMemcpyAsync for manual overlay skeleton point upload");
                instance_points_uploaded = true;
                ensure_cuda_ok(cudaMemcpyAsync(device_ptr_as_void(device_edges_upload_.data()),
                                               host_edges_upload_.data(),
                                               packed_edge_values * sizeof(std::uint32_t),
                                               cudaMemcpyHostToDevice,
                                               target.stream),
                               "cudaMemcpyAsync for manual overlay skeleton edge upload");
                draw_skeleton(target, instance, color, device_points_upload_, device_edges_upload_);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_skeleton_rgba_pitched");
                has_content = true;
            }

            if (!instance.points.empty()) {
                const std::size_t point_value_count = instance.points.size() * 2U;
                if (!instance_points_uploaded) {
                    ensure_pinned_upload_capacity<int>(
                        point_value_count,
                        &host_points_upload_,
                        "cudaHostAlloc for manual overlay marker upload");
                    ensure_device_upload_capacity<int>(
                        point_value_count,
                        &device_points_upload_,
                        "cudaMalloc for manual overlay marker upload");
                    const std::size_t packed_value_count =
                        pack_points_xy(instance.points, host_points_upload_.data());
                    ensure_cuda_ok(cudaMemcpyAsync(device_ptr_as_void(device_points_upload_.data()),
                                                   host_points_upload_.data(),
                                                   packed_value_count * sizeof(int),
                                                   cudaMemcpyHostToDevice,
                                                   target.stream),
                                   "cudaMemcpyAsync for manual overlay point marker upload");
                }
                draw_points(target, instance, color, device_points_upload_);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_points_rgba_pitched");
                has_content = true;
            }

            if (box_has_area(instance.box)) {
                draw_box_outline(target, instance, color);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_box_outline_rgba_pitched");
                has_content = true;
            }
        }

        if (snapshot.selected_instance.has_value() &&
            *snapshot.selected_instance < snapshot.instances.size()) {
            const ManualOverlayInstance& selected = snapshot.instances[*snapshot.selected_instance];
            if (box_has_area(selected.box)) {
                draw_selection_handles(target, selected);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_selection_handles_rgba_pitched");
                has_content = true;
            }
        }

        ensure_cuda_ok(cudaEventRecord(slot->ready_event.get(), target.stream),
                       "cudaEventRecord for live manual overlay");
        slot->generation = snapshot.generation;
        slot->has_content = has_content;
        worker_runtime::publish_latest_slot(*slot, latest_overlay_index_);
    } catch (...) {
        worker_runtime::release_writing_slot(*slot);
        throw;
    }
}

LiveManualOverlayWorker::OverlaySlot* LiveManualOverlayWorker::reserve_overlay_slot() {
    return worker_runtime::reserve_writable_slot_view(overlay_slots_,
                                                      latest_overlay_index_,
                                                      "cudaEventQuery for live manual overlay slot reuse");
}

void LiveManualOverlayWorker::publish_error(std::string error_message) {
    worker_runtime::publish_error(&last_error_, std::move(error_message));
}

} // namespace mmltk::live
