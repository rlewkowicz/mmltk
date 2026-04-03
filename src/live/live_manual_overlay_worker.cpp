#include "mmltk/live/live_manual_overlay_worker.h"

#include "live/upload_buffer_utils.h"
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
    if (running()) {
        throw std::runtime_error("live manual overlay worker is already running");
    }
    if (thread_.joinable()) {
        throw std::runtime_error("live manual overlay worker must be stopped before restart");
    }
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&LiveManualOverlayWorker::worker_thread_main, this);
}

void LiveManualOverlayWorker::stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

bool LiveManualOverlayWorker::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

bool LiveManualOverlayWorker::try_acquire_overlay(OverlayView* out) {
    if (out == nullptr) {
        throw std::runtime_error("live manual overlay worker requires an overlay output");
    }
    *out = {};
    return try_acquire_latest_published_slot(overlay_slots_, latest_overlay_index_, out, [](OverlaySlot& slot) {
        return OverlayView{
            slot.slot_index,
            slot.device_buffer.data(),
            slot.device_buffer.pitch_bytes(),
            slot.device_buffer.width(),
            slot.device_buffer.height(),
            slot.ready_event.get(),
            slot.has_content,
            slot.generation,
        };
    });
}

void LiveManualOverlayWorker::release_overlay(const std::uint32_t slot_index) {
    if (slot_index >= overlay_slots_.size()) {
        throw std::runtime_error("live manual overlay worker release index out of range");
    }
    OverlaySlot& slot = *overlay_slots_[slot_index];
    release_acquired_slot(slot,
                          "live manual overlay worker release called for a slot that is not acquired");
}

LiveManualOverlayWorker::Status LiveManualOverlayWorker::snapshot_status() const {
    const std::shared_ptr<const Status> current =
        std::atomic_load_explicit(&status_, std::memory_order_acquire);
    return current ? *current : Status{};
}

void LiveManualOverlayWorker::allocate_resources() {
    ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for live manual overlay resource allocation");

    try {
        for (std::uint32_t slot_index = 0; slot_index < overlay_slots_.capacity(); ++slot_index) {
            auto slot = std::make_unique<OverlaySlot>();
            slot->slot_index = slot_index;
            slot->device_buffer.ensure_dimensions(max_capture_width_, max_capture_height_,
                                                  "cudaMallocPitch for live manual overlay");
            slot->stream.create_with_highest_priority("cudaStreamCreateWithPriority for live manual overlay");
            slot->ready_event.create(cudaEventDisableTiming,
                                     "cudaEventCreateWithFlags for live manual overlay");
            overlay_slots_.push_back(std::move(slot));
        }
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
    Status status;
    status.running = true;
    std::uint64_t last_generation = 0;

    try {
        ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for live manual overlay thread");
        while (!stop_requested_.load(std::memory_order_acquire)) {
            const std::shared_ptr<const ManualOverlayDocumentSnapshot> snapshot =
                document_.snapshot_if_changed(last_generation);
            if (!snapshot) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            render_snapshot(*snapshot);
            last_generation = snapshot->generation;
            status.generations_rendered += 1U;
            status.last_generation = last_generation;
            status.last_error.clear();
            std::shared_ptr<const Status> immutable = std::make_shared<Status>(status);
            std::atomic_store_explicit(&status_, std::move(immutable), std::memory_order_release);
        }
    } catch (const std::exception& error) {
        publish_error(error.what());
        status.last_error = error.what();
        log_live_manual_overlay_message("error", error.what());
    }

    status.running = false;
    std::shared_ptr<const Status> immutable = std::make_shared<Status>(status);
    std::atomic_store_explicit(&status_, std::move(immutable), std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

void LiveManualOverlayWorker::render_snapshot(const ManualOverlayDocumentSnapshot& snapshot) {
    OverlaySlot* slot = reserve_overlay_slot();
    if (slot == nullptr) {
        return;
    }

    const std::size_t full_width_bytes = static_cast<std::size_t>(slot->device_buffer.width()) * 4U;
    ensure_cuda_ok(cudaMemset2DAsync(device_ptr_as_void(slot->device_buffer.data()),
                                     slot->device_buffer.pitch_bytes(),
                                     0,
                                     full_width_bytes,
                                     slot->device_buffer.height(),
                                     slot->stream.get()),
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
                                           slot->stream.get()),
                           "cudaMemcpyAsync for manual overlay mask upload");

            std::uint8_t* overlay_region =
                device_ptr_as_bytes(slot->device_buffer.data()) +
                static_cast<std::size_t>(instance.mask_region.capture_y) * slot->device_buffer.pitch_bytes() +
                static_cast<std::size_t>(instance.mask_region.capture_x) * 4U;
            mmltk::rfdetr::launch_draw_manual_mask_rgba_pitched(
                overlay_region,
                slot->device_buffer.pitch_bytes(),
                static_cast<int>(instance.mask_region.width),
                static_cast<int>(instance.mask_region.height),
                device_ptr_as_bytes(device_mask_upload_.data()),
                color[0],
                color[1],
                color[2],
                static_cast<std::uint8_t>(97U),
                slot->stream.get());
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
                                           slot->stream.get()),
                           "cudaMemcpyAsync for manual overlay polyline upload");
            mmltk::rfdetr::launch_draw_polyline_rgba_pitched(
                device_ptr_as_bytes(slot->device_buffer.data()),
                slot->device_buffer.pitch_bytes(),
                static_cast<int>(slot->device_buffer.width()),
                static_cast<int>(slot->device_buffer.height()),
                reinterpret_cast<const int*>(device_ptr_as_void(device_points_upload_.data())),
                static_cast<int>(instance.polyline_points.size()),
                instance.polyline_closed,
                color[0],
                color[1],
                color[2],
                2,
                slot->stream.get());
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
                                           slot->stream.get()),
                           "cudaMemcpyAsync for manual overlay skeleton point upload");
            instance_points_uploaded = true;
            ensure_cuda_ok(cudaMemcpyAsync(device_ptr_as_void(device_edges_upload_.data()),
                                           host_edges_upload_.data(),
                                           packed_edge_values * sizeof(std::uint32_t),
                                           cudaMemcpyHostToDevice,
                                           slot->stream.get()),
                           "cudaMemcpyAsync for manual overlay skeleton edge upload");
            mmltk::rfdetr::launch_draw_skeleton_rgba_pitched(
                device_ptr_as_bytes(slot->device_buffer.data()),
                slot->device_buffer.pitch_bytes(),
                static_cast<int>(slot->device_buffer.width()),
                static_cast<int>(slot->device_buffer.height()),
                reinterpret_cast<const int*>(device_ptr_as_void(device_points_upload_.data())),
                static_cast<int>(instance.points.size()),
                reinterpret_cast<const std::uint32_t*>(device_ptr_as_void(device_edges_upload_.data())),
                static_cast<int>(instance.skeleton_edges.size()),
                color[0],
                color[1],
                color[2],
                2,
                slot->stream.get());
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
                                               slot->stream.get()),
                               "cudaMemcpyAsync for manual overlay point marker upload");
            }
            mmltk::rfdetr::launch_draw_points_rgba_pitched(
                device_ptr_as_bytes(slot->device_buffer.data()),
                slot->device_buffer.pitch_bytes(),
                static_cast<int>(slot->device_buffer.width()),
                static_cast<int>(slot->device_buffer.height()),
                reinterpret_cast<const int*>(device_ptr_as_void(device_points_upload_.data())),
                static_cast<int>(instance.points.size()),
                3,
                color[0],
                color[1],
                color[2],
                static_cast<std::uint8_t>(240U),
                slot->stream.get());
            ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_points_rgba_pitched");
            has_content = true;
        }

        if (box_has_area(instance.box)) {
            mmltk::rfdetr::launch_draw_box_outline_rgba_pitched(
                device_ptr_as_bytes(slot->device_buffer.data()),
                slot->device_buffer.pitch_bytes(),
                static_cast<int>(slot->device_buffer.width()),
                static_cast<int>(slot->device_buffer.height()),
                instance.box.x1,
                instance.box.y1,
                instance.box.x2,
                instance.box.y2,
                color[0],
                color[1],
                color[2],
                2,
                slot->stream.get());
            ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_box_outline_rgba_pitched");
            has_content = true;
        }
    }

    if (snapshot.selected_instance.has_value() &&
        *snapshot.selected_instance < snapshot.instances.size()) {
        const ManualOverlayInstance& selected = snapshot.instances[*snapshot.selected_instance];
        if (box_has_area(selected.box)) {
            mmltk::rfdetr::launch_draw_selection_handles_rgba_pitched(
                device_ptr_as_bytes(slot->device_buffer.data()),
                slot->device_buffer.pitch_bytes(),
                static_cast<int>(slot->device_buffer.width()),
                static_cast<int>(slot->device_buffer.height()),
                selected.box.x1,
                selected.box.y1,
                selected.box.x2,
                selected.box.y2,
                4,
                slot->stream.get());
            ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_selection_handles_rgba_pitched");
            has_content = true;
        }
    }

    ensure_cuda_ok(cudaEventRecord(slot->ready_event.get(), slot->stream.get()),
                   "cudaEventRecord for live manual overlay");
    slot->generation = snapshot.generation;
    slot->has_content = has_content;
    slot->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
    latest_overlay_index_.store(static_cast<int>(slot->slot_index), std::memory_order_release);
}

LiveManualOverlayWorker::OverlaySlot* LiveManualOverlayWorker::reserve_overlay_slot() {
    return reserve_writable_slot(
        overlay_slots_,
        latest_overlay_index_,
        [](OverlaySlot& slot) {
            slot.generation = 0;
            slot.has_content = false;
        },
        [](OverlaySlot& slot) { return slot.ready_event.get(); },
        "cudaEventQuery for live manual overlay slot reuse");
}

bool LiveManualOverlayWorker::try_acquire_overlay_slot(OverlaySlot& slot, OverlayView* out) {
    return try_acquire_published_slot(slot, out, [](OverlaySlot& published) {
        return OverlayView{
            published.slot_index,
            published.device_buffer.data(),
            published.device_buffer.pitch_bytes(),
            published.device_buffer.width(),
            published.device_buffer.height(),
            published.ready_event.get(),
            published.has_content,
            published.generation,
        };
    });
}

void LiveManualOverlayWorker::publish_error(std::string error_message) {
    store_error_message(&last_error_, std::move(error_message));
}

} // namespace mmltk::live
