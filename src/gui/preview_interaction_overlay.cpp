#include "preview_interaction_overlay.h"

#include "cuda_gl_interop_utils.h"
#include "live/upload_buffer_utils.h"
#include "mmltk/rfdetr/draw_cuda.h"
#include "overlay_compare_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace mmltk::gui {

namespace {

using mmltk::live::ensure_cuda_ok;
using mmltk::live::ensure_device_upload_capacity;
using mmltk::live::ensure_pinned_upload_capacity;
using mmltk::live::event_ready;
using mmltk::live::pack_edge_indices;
using mmltk::live::pack_points_xy;

constexpr std::size_t kRgba4BytesPerPixel = 4U;
constexpr std::uint32_t kOverlaySlotCount = 2U;

struct OverlayTarget {
    std::uint8_t* data;
    std::size_t pitch;
    int width;
    int height;
    cudaStream_t stream;
};

void draw_box_outline(const OverlayTarget& target, const PreviewInteractionOverlayBox& box) {
    mmltk::rfdetr::launch_draw_box_outline_rgba_pitched(
        target.data, target.pitch, target.width, target.height,
        box.box.x1, box.box.y1, box.box.x2, box.box.y2,
        box.r, box.g, box.b, std::max(1, box.thickness), target.stream);
}

void draw_selection_handles(const OverlayTarget& target, const PreviewInteractionOverlayBox& box) {
    mmltk::rfdetr::launch_draw_selection_handles_rgba_pitched(
        target.data, target.pitch, target.width, target.height,
        box.box.x1, box.box.y1, box.box.x2, box.box.y2,
        std::max(1, box.handle_radius), target.stream);
}

void draw_polyline(const OverlayTarget& target,
                   const PreviewInteractionOverlayPolyline& polyline,
                   const mmltk::live::DeviceUploadBuffer<int>& points_upload) {
    mmltk::rfdetr::launch_draw_polyline_rgba_pitched(
        target.data, target.pitch, target.width, target.height,
        reinterpret_cast<const int*>(mmltk::live::device_ptr_as_void(points_upload.data())),
        static_cast<int>(polyline.points.size()),
        polyline.closed,
        polyline.r, polyline.g, polyline.b, std::max(1, polyline.thickness), target.stream);
}

void draw_skeleton(const OverlayTarget& target,
                   const PreviewInteractionOverlaySkeleton& skeleton,
                   const mmltk::live::DeviceUploadBuffer<int>& points_upload,
                   const mmltk::live::DeviceUploadBuffer<std::uint32_t>& edges_upload) {
    mmltk::rfdetr::launch_draw_skeleton_rgba_pitched(
        target.data, target.pitch, target.width, target.height,
        reinterpret_cast<const int*>(mmltk::live::device_ptr_as_void(points_upload.data())),
        static_cast<int>(skeleton.points.size()),
        reinterpret_cast<const std::uint32_t*>(mmltk::live::device_ptr_as_void(edges_upload.data())),
        static_cast<int>(skeleton.edges.size()),
        skeleton.r, skeleton.g, skeleton.b, std::max(1, skeleton.thickness), target.stream);
}

void draw_points(const OverlayTarget& target,
                 const PreviewInteractionOverlayMarkerSet& marker_set,
                 const mmltk::live::DeviceUploadBuffer<int>& points_upload) {
    mmltk::rfdetr::launch_draw_points_rgba_pitched(
        target.data, target.pitch, target.width, target.height,
        reinterpret_cast<const int*>(mmltk::live::device_ptr_as_void(points_upload.data())),
        static_cast<int>(marker_set.points.size()),
        std::max(1, marker_set.radius),
        marker_set.r, marker_set.g, marker_set.b, marker_set.alpha, target.stream);
}

bool annotation_box_has_area(const mmltk::live::ManualOverlayBox& box) {
    return box.x2 > box.x1 && box.y2 > box.y1;
}

} // namespace

PreviewInteractionOverlaySurface::PreviewInteractionOverlaySurface(GLFWwindow* shared_context_window)
    : shared_context_window_(shared_context_window) {}

PreviewInteractionOverlaySurface::~PreviewInteractionOverlaySurface() {
    shutdown();
}

bool PreviewInteractionOverlaySurface::initialize(std::string* error_message) {
    shutdown();

    if (shared_context_window_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "interaction overlay requires a valid GLFW window";
        }
        return false;
    }

    std::string initialize_error;
    const bool ok = initialize_gl_resources(&initialize_error);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        initialized_ = ok;
        state_.initialized = ok;
        state_.last_error = initialize_error;
        state_.interop_failed = is_invalid_graphics_context_error(initialize_error);
    }
    if (!ok) {
        if (error_message != nullptr) {
            *error_message = initialize_error;
        }
        return false;
    }

    stop_requested_.store(false, std::memory_order_release);
    worker_thread_ = std::thread(&PreviewInteractionOverlaySurface::worker_thread_main, this);
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void PreviewInteractionOverlaySurface::shutdown() {
    stop_requested_.store(true, std::memory_order_release);
    snapshot_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    destroy_overlay_slots();
    destroy_upload_resources();
    destroy_gl_resources();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {};
        initialized_ = false;
        pending_upload_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        latest_snapshot_.reset();
        next_generation_ = 1;
    }
    latest_overlay_index_.store(-1, std::memory_order_release);
}

void PreviewInteractionOverlaySurface::pump() {
    if (!initialized_) {
        return;
    }

    std::string overlay_error;
    if (!finalize_pending_upload(&overlay_error)) {
        set_error(overlay_error, is_invalid_graphics_context_error(overlay_error));
        return;
    }
    clear_error();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_upload_.has_value()) {
            return;
        }
    }

    OverlaySlot* slot = nullptr;
    const int latest_index = latest_overlay_index_.load(std::memory_order_acquire);
    if (latest_index >= 0 && latest_index < static_cast<int>(overlay_slots_.size())) {
        (void)try_acquire_overlay_slot(*overlay_slots_[static_cast<std::size_t>(latest_index)], &slot);
    }
    if (slot == nullptr) {
        for (auto& candidate : overlay_slots_) {
            if (static_cast<int>(candidate->slot_index) == latest_index) {
                continue;
            }
            if (try_acquire_overlay_slot(*candidate, &slot)) {
                break;
            }
        }
    }
    if (slot == nullptr) {
        return;
    }

    if (!stage_pending_upload(*slot, &overlay_error)) {
        release_overlay_slot(slot->slot_index);
        set_error(overlay_error, is_invalid_graphics_context_error(overlay_error));
    }
}

void PreviewInteractionOverlaySurface::publish_snapshot(PreviewInteractionOverlaySnapshot snapshot) {
    if (snapshot.width == 0U || snapshot.height == 0U) {
        return;
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (latest_snapshot_ != nullptr && mmltk::overlay_compare::preview_snapshot_equals(snapshot, *latest_snapshot_)) {
        return;
    }
    snapshot.generation = next_generation_++;
    latest_snapshot_ = std::make_shared<PreviewInteractionOverlaySnapshot>(std::move(snapshot));
    snapshot_cv_.notify_one();
}

PreviewInteractionOverlayState PreviewInteractionOverlaySurface::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool PreviewInteractionOverlaySurface::initialize_gl_resources(std::string* error_message) {
    return interop_core_.initialize(error_message);
}

void PreviewInteractionOverlaySurface::destroy_gl_resources() {
    interop_core_.shutdown();
}

bool PreviewInteractionOverlaySurface::ensure_texture_storage(const std::uint32_t width,
                                                              const std::uint32_t height,
                                                              std::string* error_message) {
    return interop_core_.ensure_texture_storage(width, height, error_message);
}

bool PreviewInteractionOverlaySurface::ensure_upload_resources(const std::uint32_t width,
                                                               const std::uint32_t height,
                                                               const int cuda_device_index,
                                                               std::string* error_message) {
    return interop_core_.ensure_upload_resources(width, height, cuda_device_index, error_message);
}

void PreviewInteractionOverlaySurface::destroy_upload_resources() {
    interop_core_.reset_upload_resources();
}

bool PreviewInteractionOverlaySurface::stage_pending_upload(const OverlaySlot& slot, std::string* error_message) {
    if (!ensure_upload_resources(slot.width, slot.height, slot.cuda_device_index, error_message)) {
        return false;
    }

    if (!interop_core_.set_cuda_device(error_message)) {
        return false;
    }

    cudaError_t cuda_status = cudaSuccess;
    const char* failed_cuda_label = nullptr;
    if (slot.ready_event != nullptr) {
        failed_cuda_label = "cudaStreamWaitEvent";
        cuda_status = cudaStreamWaitEvent(interop_core_.stream(), slot.ready_event, 0);
    }

    bool mapped = false;
    void* pixel_buffer_ptr = nullptr;
    std::size_t mapped_size = 0U;
    if (cuda_status == cudaSuccess) {
        failed_cuda_label = "cudaGraphicsMapResources";
        cuda_status =
            cudaGraphicsMapResources(1, interop_core_.graphics_resource_ptr(), interop_core_.stream());
        mapped = cuda_status == cudaSuccess;
    }
    if (cuda_status == cudaSuccess) {
        failed_cuda_label = "cudaGraphicsResourceGetMappedPointer";
        cuda_status = cudaGraphicsResourceGetMappedPointer(
            &pixel_buffer_ptr,
            &mapped_size,
            interop_core_.graphics_resource());
    }

    const std::size_t copy_width_bytes = static_cast<std::size_t>(slot.width) * kRgba4BytesPerPixel;
    const std::size_t copy_height = static_cast<std::size_t>(slot.height);
    const std::size_t required_bytes = copy_width_bytes * copy_height;
    if (cuda_status == cudaSuccess && mapped_size < required_bytes) {
        cuda_status = cudaErrorInvalidValue;
        failed_cuda_label = "cudaGraphicsResourceGetMappedPointer";
    }
    if (cuda_status == cudaSuccess) {
        failed_cuda_label = "cudaMemcpy2DAsync";
        cuda_status = cudaMemcpy2DAsync(pixel_buffer_ptr,
                                        copy_width_bytes,
                                        mmltk::live::device_ptr_as_const_void(slot.device_ptr),
                                        slot.pitch_bytes,
                                        copy_width_bytes,
                                        copy_height,
                                        cudaMemcpyDeviceToDevice,
                                        interop_core_.stream());
    }

    const cudaError_t unmap_status =
        mapped ? cudaGraphicsUnmapResources(1,
                                            interop_core_.graphics_resource_ptr(),
                                            interop_core_.stream())
               : cudaSuccess;

    if (cuda_status != cudaSuccess) {
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(cuda_status, failed_cuda_label);
        }
        return false;
    }
    if (unmap_status != cudaSuccess) {
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(unmap_status, "cudaGraphicsUnmapResources");
        }
        return false;
    }

    cuda_status = cudaEventRecord(interop_core_.completion_event(),
                                  interop_core_.stream());
    if (cuda_status != cudaSuccess) {
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(cuda_status, "cudaEventRecord");
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pending_upload_ = PendingUpload{
        slot.slot_index,
        slot.width,
        slot.height,
        slot.cuda_device_index,
        slot.generation,
    };
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

bool PreviewInteractionOverlaySurface::finalize_pending_upload(std::string* error_message) {
    std::optional<PendingUpload> pending_upload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_upload_.has_value()) {
            if (error_message != nullptr) {
                error_message->clear();
            }
            return true;
        }
        pending_upload = pending_upload_;
    }
    if (!pending_upload.has_value()) {
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
    }

    if (!interop_core_.set_cuda_device(error_message)) {
        return false;
    }

    const cudaError_t query_status =
        cudaEventQuery(interop_core_.completion_event());
    if (query_status == cudaErrorNotReady) {
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
    }
    if (query_status != cudaSuccess) {
        release_overlay_slot(pending_upload->slot_index);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_upload_.reset();
        }
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(query_status, "cudaEventQuery");
        }
        return false;
    }

    if (!interop_core_.update_back_texture_from_pixel_buffer(
            pending_upload->width,
            pending_upload->height,
            error_message)) {
        release_overlay_slot(pending_upload->slot_index);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_upload_.reset();
        }
        return false;
    }

    release_overlay_slot(pending_upload->slot_index);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_upload_.reset();
    }
    publish_ready_texture(pending_upload->width, pending_upload->height);
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void PreviewInteractionOverlaySurface::publish_ready_texture(const std::uint32_t width,
                                                             const std::uint32_t height) {
    interop_core_.swap_ready_texture();

    std::lock_guard<std::mutex> lock(mutex_);
    state_.has_frame = true;
    state_.texture_id = interop_core_.front_texture_id();
    state_.width = width;
    state_.height = height;
}

void PreviewInteractionOverlaySurface::set_error(const std::string& error_message, const bool interop_failed) {
    if (error_message.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.last_error != error_message) {
        log_gui_surface_error("interaction overlay", error_message);
    }
    state_.last_error = error_message;
    state_.interop_failed = interop_failed;
}

void PreviewInteractionOverlaySurface::clear_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.last_error.clear();
    state_.interop_failed = false;
}

void PreviewInteractionOverlaySurface::worker_thread_main() {
    std::uint64_t last_generation = 0;

    try {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            std::shared_ptr<const PreviewInteractionOverlaySnapshot> snapshot;
            {
                std::unique_lock<std::mutex> lock(snapshot_mutex_);
                snapshot_cv_.wait(lock, [this, last_generation]() {
                    return stop_requested_.load(std::memory_order_acquire) ||
                           (latest_snapshot_ != nullptr && latest_snapshot_->generation != last_generation);
                });
                if (stop_requested_.load(std::memory_order_acquire)) {
                    break;
                }
                snapshot = latest_snapshot_;
            }
            if (snapshot == nullptr || snapshot->generation == last_generation) {
                continue;
            }
            render_snapshot(*snapshot);
            last_generation = snapshot->generation;
        }
    } catch (const std::exception& error) {
        set_error(error.what(), false);
    }
}

void PreviewInteractionOverlaySurface::render_snapshot(const PreviewInteractionOverlaySnapshot& snapshot) {
    if (worker_cuda_device_index_ >= 0 && worker_cuda_device_index_ != snapshot.cuda_device_index) {
        destroy_overlay_slots();
    }
    OverlaySlot* slot = reserve_overlay_slot();
    if (slot == nullptr) {
        return;
    }

    if (!ensure_overlay_slot_capacity(*slot, snapshot.width, snapshot.height, snapshot.cuda_device_index)) {
        slot->state.store(mmltk::live::to_slot_state_value(mmltk::live::SlotState::kFree), std::memory_order_release);
        return;
    }

    const OverlayTarget target{
        mmltk::live::device_ptr_as_bytes(slot->device_ptr),
        slot->pitch_bytes,
        static_cast<int>(snapshot.width),
        static_cast<int>(snapshot.height),
        slot->stream
    };

    ensure_cuda_ok(cudaMemset2DAsync(mmltk::live::device_ptr_as_void(slot->device_ptr),
                                     target.pitch,
                                     0,
                                     static_cast<std::size_t>(target.width) * kRgba4BytesPerPixel,
                                     target.height,
                                     target.stream),
                   "cudaMemset2DAsync for interaction overlay clear");

    for (const PreviewInteractionOverlayBox& box : snapshot.boxes) {
        if (!annotation_box_has_area(box.box)) {
            continue;
        }
        draw_box_outline(target, box);
        ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_box_outline_rgba_pitched for interaction overlay");
        if (box.draw_handles) {
            draw_selection_handles(target, box);
            ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_selection_handles_rgba_pitched for interaction overlay");
        }
    }

    for (const PreviewInteractionOverlayPolyline& polyline : snapshot.polylines) {
        if (polyline.points.size() < 2U) {
            continue;
        }
        const std::size_t point_value_count = polyline.points.size() * 2U;
        ensure_pinned_upload_capacity<int>(
            point_value_count,
            &host_points_upload_,
            "cudaHostAlloc for interaction overlay polyline upload");
        ensure_device_upload_capacity<int>(
            point_value_count,
            &device_points_upload_,
            "cudaMalloc for interaction overlay polyline upload");
        const std::size_t packed_value_count = pack_points_xy(polyline.points, host_points_upload_.data());
        ensure_cuda_ok(cudaMemcpyAsync(mmltk::live::device_ptr_as_void(device_points_upload_.data()),
                                       host_points_upload_.data(),
                                       packed_value_count * sizeof(int),
                                       cudaMemcpyHostToDevice,
                                       target.stream),
                       "cudaMemcpyAsync for interaction overlay polyline upload");
        draw_polyline(target, polyline, device_points_upload_);
        ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_polyline_rgba_pitched for interaction overlay");
    }

    for (const PreviewInteractionOverlaySkeleton& skeleton : snapshot.skeletons) {
        if (skeleton.points.empty() || skeleton.edges.empty()) {
            continue;
        }
        const std::size_t point_value_count = skeleton.points.size() * 2U;
        const std::size_t edge_value_count = skeleton.edges.size() * 2U;
        ensure_pinned_upload_capacity<int>(
            point_value_count,
            &host_points_upload_,
            "cudaHostAlloc for interaction overlay skeleton point upload");
        ensure_device_upload_capacity<int>(
            point_value_count,
            &device_points_upload_,
            "cudaMalloc for interaction overlay skeleton point upload");
        ensure_pinned_upload_capacity<std::uint32_t>(
            edge_value_count,
            &host_edges_upload_,
            "cudaHostAlloc for interaction overlay skeleton edge upload");
        ensure_device_upload_capacity<std::uint32_t>(
            edge_value_count,
            &device_edges_upload_,
            "cudaMalloc for interaction overlay skeleton edge upload");
        const std::size_t packed_point_values = pack_points_xy(skeleton.points, host_points_upload_.data());
        const std::size_t packed_edge_values = pack_edge_indices(skeleton.edges, host_edges_upload_.data());
        ensure_cuda_ok(cudaMemcpyAsync(mmltk::live::device_ptr_as_void(device_points_upload_.data()),
                                       host_points_upload_.data(),
                                       packed_point_values * sizeof(int),
                                       cudaMemcpyHostToDevice,
                                       target.stream),
                       "cudaMemcpyAsync for interaction overlay skeleton point upload");
        ensure_cuda_ok(cudaMemcpyAsync(mmltk::live::device_ptr_as_void(device_edges_upload_.data()),
                                       host_edges_upload_.data(),
                                       packed_edge_values * sizeof(std::uint32_t),
                                       cudaMemcpyHostToDevice,
                                       target.stream),
                       "cudaMemcpyAsync for interaction overlay skeleton edge upload");
        draw_skeleton(target, skeleton, device_points_upload_, device_edges_upload_);
        ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_skeleton_rgba_pitched for interaction overlay");
    }

    for (const PreviewInteractionOverlayMarkerSet& marker_set : snapshot.marker_sets) {
        if (marker_set.points.empty()) {
            continue;
        }
        const std::size_t point_value_count = marker_set.points.size() * 2U;
        ensure_pinned_upload_capacity<int>(
            point_value_count,
            &host_points_upload_,
            "cudaHostAlloc for interaction overlay marker upload");
        ensure_device_upload_capacity<int>(
            point_value_count,
            &device_points_upload_,
            "cudaMalloc for interaction overlay marker upload");
        const std::size_t packed_value_count = pack_points_xy(marker_set.points, host_points_upload_.data());
        ensure_cuda_ok(cudaMemcpyAsync(mmltk::live::device_ptr_as_void(device_points_upload_.data()),
                                       host_points_upload_.data(),
                                       packed_value_count * sizeof(int),
                                       cudaMemcpyHostToDevice,
                                       target.stream),
                       "cudaMemcpyAsync for interaction overlay marker upload");
        draw_points(target, marker_set, device_points_upload_);
        ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_points_rgba_pitched for interaction overlay");
    }

    ensure_cuda_ok(cudaEventRecord(slot->ready_event, target.stream),
                   "cudaEventRecord for interaction overlay");
    slot->width = snapshot.width;
    slot->height = snapshot.height;
    slot->cuda_device_index = snapshot.cuda_device_index;
    slot->generation = snapshot.generation;
    slot->state.store(mmltk::live::to_slot_state_value(mmltk::live::SlotState::kPublished), std::memory_order_release);
    latest_overlay_index_.store(static_cast<int>(slot->slot_index), std::memory_order_release);
}

bool PreviewInteractionOverlaySurface::ensure_overlay_slot_capacity(OverlaySlot& slot,
                                                                    const std::uint32_t width,
                                                                    const std::uint32_t height,
                                                                    const int cuda_device_index) {
    const bool size_changed = slot.capacity_width < width || slot.capacity_height < height;
    if (worker_cuda_device_index_ != cuda_device_index) {
        ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for interaction overlay worker");
        worker_cuda_device_index_ = cuda_device_index;
    }

    if (!size_changed && slot.device_ptr != 0) {
        return true;
    }

    if (slot.ready_event != nullptr) {
        (void)cudaEventDestroy(slot.ready_event);
        slot.ready_event = nullptr;
    }
    if (slot.stream != nullptr) {
        (void)cudaStreamDestroy(slot.stream);
        slot.stream = nullptr;
    }
    if (slot.device_ptr != 0) {
        (void)cudaFree(mmltk::live::device_ptr_as_void(slot.device_ptr));
        slot.device_ptr = 0;
    }

    ensure_cuda_ok(cudaMallocPitch(reinterpret_cast<void**>(&slot.device_ptr),
                                   &slot.pitch_bytes,
                                   static_cast<std::size_t>(width) * kRgba4BytesPerPixel,
                                   height),
                   "cudaMallocPitch for interaction overlay slot");
    ensure_cuda_ok(mmltk::cuda_stream_create_with_highest_priority(&slot.stream, cudaStreamNonBlocking),
                   "cudaStreamCreateWithPriority for interaction overlay slot");
    ensure_cuda_ok(cudaEventCreateWithFlags(&slot.ready_event, cudaEventDisableTiming),
                   "cudaEventCreateWithFlags for interaction overlay slot");
    slot.capacity_width = width;
    slot.capacity_height = height;
    return true;
}

void PreviewInteractionOverlaySurface::destroy_overlay_slots() noexcept {
    if (worker_cuda_device_index_ >= 0) {
        (void)cudaSetDevice(worker_cuda_device_index_);
    }
    for (auto& slot : overlay_slots_) {
        if (slot->ready_event != nullptr) {
            (void)cudaEventDestroy(slot->ready_event);
            slot->ready_event = nullptr;
        }
        if (slot->stream != nullptr) {
            (void)cudaStreamDestroy(slot->stream);
            slot->stream = nullptr;
        }
        if (slot->device_ptr != 0) {
            (void)cudaFree(mmltk::live::device_ptr_as_void(slot->device_ptr));
            slot->device_ptr = 0;
        }
        slot->capacity_width = 0;
        slot->capacity_height = 0;
    }
    host_points_upload_.reset();
    device_points_upload_.reset();
    host_edges_upload_.reset();
    device_edges_upload_.reset();
    overlay_slots_.clear();
    latest_overlay_index_.store(-1, std::memory_order_release);
    worker_cuda_device_index_ = -1;
}

PreviewInteractionOverlaySurface::OverlaySlot* PreviewInteractionOverlaySurface::reserve_overlay_slot() {
    if (overlay_slots_.empty()) {
        overlay_slots_.reserve(kOverlaySlotCount);
        for (std::uint32_t slot_index = 0; slot_index < kOverlaySlotCount; ++slot_index) {
            auto slot = std::make_unique<OverlaySlot>();
            slot->slot_index = slot_index;
            overlay_slots_.push_back(std::move(slot));
        }
    }

    for (auto& slot : overlay_slots_) {
        std::uint32_t expected = mmltk::live::to_slot_state_value(mmltk::live::SlotState::kFree);
        if (slot->state.compare_exchange_strong(expected,
                                                mmltk::live::to_slot_state_value(mmltk::live::SlotState::kWriting),
                                                std::memory_order_acq_rel)) {
            return slot.get();
        }

        if (expected != mmltk::live::to_slot_state_value(mmltk::live::SlotState::kPublished) ||
            !event_ready(slot->ready_event, "cudaEventQuery for interaction overlay slot reuse")) {
            continue;
        }
        expected = mmltk::live::to_slot_state_value(mmltk::live::SlotState::kPublished);
        if (slot->state.compare_exchange_strong(expected,
                                                mmltk::live::to_slot_state_value(mmltk::live::SlotState::kWriting),
                                                std::memory_order_acq_rel)) {
            if (latest_overlay_index_.load(std::memory_order_acquire) == static_cast<int>(slot->slot_index)) {
                latest_overlay_index_.store(-1, std::memory_order_release);
            }
            return slot.get();
        }
    }
    return nullptr;
}

bool PreviewInteractionOverlaySurface::try_acquire_overlay_slot(OverlaySlot& slot, OverlaySlot** out) {
    std::uint32_t expected = mmltk::live::to_slot_state_value(mmltk::live::SlotState::kPublished);
    if (!slot.state.compare_exchange_strong(expected,
                                            mmltk::live::to_slot_state_value(mmltk::live::SlotState::kAcquired),
                                            std::memory_order_acq_rel)) {
        return false;
    }
    *out = &slot;
    return true;
}

void PreviewInteractionOverlaySurface::release_overlay_slot(const std::uint32_t slot_index) {
    if (slot_index >= overlay_slots_.size()) {
        return;
    }
    OverlaySlot& slot = *overlay_slots_[slot_index];
    std::uint32_t expected = mmltk::live::to_slot_state_value(mmltk::live::SlotState::kAcquired);
    (void)slot.state.compare_exchange_strong(expected,
                                             mmltk::live::to_slot_state_value(mmltk::live::SlotState::kFree),
                                             std::memory_order_acq_rel);
}

} // namespace mmltk::gui
