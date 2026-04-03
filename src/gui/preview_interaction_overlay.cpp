#include "preview_interaction_overlay.h"

#include "cuda_gl_interop_utils.h"
#include "cuda_priority.h"
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
        texture_width_ = 0;
        texture_height_ = 0;
        front_texture_index_ = 0;
        back_texture_index_ = 1;
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
    destroy_gl_resources();
    initialize_gui_texture_pair(textures_);
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void PreviewInteractionOverlaySurface::destroy_gl_resources() {
    destroy_gui_texture_pair(textures_);
}

bool PreviewInteractionOverlaySurface::ensure_texture_storage(const std::uint32_t width,
                                                              const std::uint32_t height,
                                                              std::string* error_message) {
    return ensure_gui_texture_storage(textures_,
                                      width,
                                      height,
                                      GL_RGBA8,
                                      GL_RGBA,
                                      "interaction overlay dimensions must be non-zero",
                                      texture_width_,
                                      texture_height_,
                                      error_message);
}

bool PreviewInteractionOverlaySurface::ensure_upload_resources(const std::uint32_t width,
                                                               const std::uint32_t height,
                                                               const int cuda_device_index,
                                                               std::string* error_message) {
    const bool storage_changed = texture_width_ != width || texture_height_ != height;
    if (upload_cuda_device_index_ >= 0 && upload_cuda_device_index_ != cuda_device_index) {
        destroy_upload_resources();
    }

    if (!ensure_texture_storage(width, height, error_message)) {
        return false;
    }

    cudaError_t cuda_status = cudaSetDevice(cuda_device_index);
    if (cuda_status != cudaSuccess) {
        if (error_message != nullptr) {
            *error_message = cuda_gl_interop_error_message(cuda_status, "cudaSetDevice");
        }
        return false;
    }

    if (upload_stream_ == nullptr) {
        cuda_status = mmltk::cuda_stream_create_with_highest_priority(&upload_stream_, cudaStreamNonBlocking);
        if (cuda_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(cuda_status, "cudaStreamCreateWithPriority");
            }
            return false;
        }
    }

    if (upload_complete_event_ == nullptr) {
        cuda_status = cudaEventCreateWithFlags(&upload_complete_event_, cudaEventDisableTiming);
        if (cuda_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(cuda_status, "cudaEventCreateWithFlags");
            }
            return false;
        }
    }

    if (pixel_buffer_ == 0U) {
        glGenBuffers(1, &pixel_buffer_);
    }

    if (graphics_resource_ != nullptr && storage_changed) {
        cuda_status = cudaGraphicsUnregisterResource(graphics_resource_);
        if (cuda_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(cuda_status, "cudaGraphicsUnregisterResource");
            }
            return false;
        }
        graphics_resource_ = nullptr;
    }

    if (graphics_resource_ == nullptr) {
        const std::size_t required_bytes =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kRgba4BytesPerPixel;
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pixel_buffer_);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(required_bytes), nullptr, GL_STREAM_DRAW);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        cuda_status =
            cudaGraphicsGLRegisterBuffer(&graphics_resource_, pixel_buffer_, cudaGraphicsRegisterFlagsWriteDiscard);
        if (cuda_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(cuda_status, "cudaGraphicsGLRegisterBuffer");
            }
            return false;
        }
    }

    upload_cuda_device_index_ = cuda_device_index;
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void PreviewInteractionOverlaySurface::destroy_upload_resources() {
    if (upload_cuda_device_index_ >= 0) {
        (void)cudaSetDevice(upload_cuda_device_index_);
    }
    if (upload_stream_ != nullptr) {
        (void)cudaStreamSynchronize(upload_stream_);
    }
    if (graphics_resource_ != nullptr) {
        (void)cudaGraphicsUnregisterResource(graphics_resource_);
        graphics_resource_ = nullptr;
    }
    if (upload_complete_event_ != nullptr) {
        (void)cudaEventDestroy(upload_complete_event_);
        upload_complete_event_ = nullptr;
    }
    if (upload_stream_ != nullptr) {
        (void)cudaStreamDestroy(upload_stream_);
        upload_stream_ = nullptr;
    }
    if (pixel_buffer_ != 0U) {
        glDeleteBuffers(1, &pixel_buffer_);
        pixel_buffer_ = 0U;
    }
    upload_cuda_device_index_ = -1;
}

bool PreviewInteractionOverlaySurface::stage_pending_upload(const OverlaySlot& slot, std::string* error_message) {
    if (!ensure_upload_resources(slot.width, slot.height, slot.cuda_device_index, error_message)) {
        return false;
    }

    if (upload_cuda_device_index_ >= 0) {
        const cudaError_t set_device_status = cudaSetDevice(upload_cuda_device_index_);
        if (set_device_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(set_device_status, "cudaSetDevice");
            }
            return false;
        }
    }

    cudaError_t cuda_status = cudaSuccess;
    const char* failed_cuda_label = nullptr;
    if (slot.ready_event != nullptr) {
        failed_cuda_label = "cudaStreamWaitEvent";
        cuda_status = cudaStreamWaitEvent(upload_stream_, slot.ready_event, 0);
    }

    bool mapped = false;
    void* pixel_buffer_ptr = nullptr;
    std::size_t mapped_size = 0;
    if (cuda_status == cudaSuccess) {
        failed_cuda_label = "cudaGraphicsMapResources";
        cuda_status = cudaGraphicsMapResources(1, &graphics_resource_, upload_stream_);
        mapped = cuda_status == cudaSuccess;
    }
    if (cuda_status == cudaSuccess) {
        failed_cuda_label = "cudaGraphicsResourceGetMappedPointer";
        cuda_status = cudaGraphicsResourceGetMappedPointer(&pixel_buffer_ptr, &mapped_size, graphics_resource_);
    }

    const std::size_t copy_width_bytes = static_cast<std::size_t>(slot.width) * kRgba4BytesPerPixel;
    const auto copy_height = static_cast<std::size_t>(slot.height);
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
                                        upload_stream_);
    }

    const cudaError_t unmap_status =
        mapped ? cudaGraphicsUnmapResources(1, &graphics_resource_, upload_stream_) : cudaSuccess;

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

    cuda_status = cudaEventRecord(upload_complete_event_, upload_stream_);
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

    if (upload_cuda_device_index_ >= 0) {
        const cudaError_t set_device_status = cudaSetDevice(upload_cuda_device_index_);
        if (set_device_status != cudaSuccess) {
            if (error_message != nullptr) {
                *error_message = cuda_gl_interop_error_message(set_device_status, "cudaSetDevice");
            }
            return false;
        }
    }

    const cudaError_t query_status = cudaEventQuery(upload_complete_event_);
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

    glBindTexture(GL_TEXTURE_2D, textures_[back_texture_index_]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pixel_buffer_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    static_cast<GLsizei>(pending_upload->width),
                    static_cast<GLsizei>(pending_upload->height),
                    GL_RGBA,
                    GL_UNSIGNED_BYTE,
                    nullptr);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFlush();

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
    std::swap(front_texture_index_, back_texture_index_);

    std::lock_guard<std::mutex> lock(mutex_);
    state_.has_frame = true;
    state_.texture_id = imgui_texture_id_from_gl_name(textures_[front_texture_index_]);
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

    ensure_cuda_ok(cudaMemset2DAsync(mmltk::live::device_ptr_as_void(slot->device_ptr),
                                     slot->pitch_bytes,
                                     0,
                                     static_cast<std::size_t>(snapshot.width) * kRgba4BytesPerPixel,
                                     snapshot.height,
                                     slot->stream),
                   "cudaMemset2DAsync for interaction overlay clear");

    for (const PreviewInteractionOverlayBox& box : snapshot.boxes) {
        if (!annotation_box_has_area(box.box)) {
            continue;
        }
        mmltk::rfdetr::launch_draw_box_outline_rgba_pitched(
            mmltk::live::device_ptr_as_bytes(slot->device_ptr),
            slot->pitch_bytes,
            static_cast<int>(snapshot.width),
            static_cast<int>(snapshot.height),
            box.box.x1,
            box.box.y1,
            box.box.x2,
            box.box.y2,
            box.r,
            box.g,
            box.b,
            std::max(1, box.thickness),
            slot->stream);
        ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_box_outline_rgba_pitched for interaction overlay");
        if (box.draw_handles) {
            mmltk::rfdetr::launch_draw_selection_handles_rgba_pitched(
                mmltk::live::device_ptr_as_bytes(slot->device_ptr),
                slot->pitch_bytes,
                static_cast<int>(snapshot.width),
                static_cast<int>(snapshot.height),
                box.box.x1,
                box.box.y1,
                box.box.x2,
                box.box.y2,
                std::max(1, box.handle_radius),
                slot->stream);
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
                                       slot->stream),
                       "cudaMemcpyAsync for interaction overlay polyline upload");
        mmltk::rfdetr::launch_draw_polyline_rgba_pitched(
            mmltk::live::device_ptr_as_bytes(slot->device_ptr),
            slot->pitch_bytes,
            static_cast<int>(snapshot.width),
            static_cast<int>(snapshot.height),
            reinterpret_cast<const int*>(mmltk::live::device_ptr_as_void(device_points_upload_.data())),
            static_cast<int>(polyline.points.size()),
            polyline.closed,
            polyline.r,
            polyline.g,
            polyline.b,
            std::max(1, polyline.thickness),
            slot->stream);
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
                                       slot->stream),
                       "cudaMemcpyAsync for interaction overlay skeleton point upload");
        ensure_cuda_ok(cudaMemcpyAsync(mmltk::live::device_ptr_as_void(device_edges_upload_.data()),
                                       host_edges_upload_.data(),
                                       packed_edge_values * sizeof(std::uint32_t),
                                       cudaMemcpyHostToDevice,
                                       slot->stream),
                       "cudaMemcpyAsync for interaction overlay skeleton edge upload");
        mmltk::rfdetr::launch_draw_skeleton_rgba_pitched(
            mmltk::live::device_ptr_as_bytes(slot->device_ptr),
            slot->pitch_bytes,
            static_cast<int>(snapshot.width),
            static_cast<int>(snapshot.height),
            reinterpret_cast<const int*>(mmltk::live::device_ptr_as_void(device_points_upload_.data())),
            static_cast<int>(skeleton.points.size()),
            reinterpret_cast<const std::uint32_t*>(mmltk::live::device_ptr_as_void(device_edges_upload_.data())),
            static_cast<int>(skeleton.edges.size()),
            skeleton.r,
            skeleton.g,
            skeleton.b,
            std::max(1, skeleton.thickness),
            slot->stream);
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
                                       slot->stream),
                       "cudaMemcpyAsync for interaction overlay marker upload");
        mmltk::rfdetr::launch_draw_points_rgba_pitched(
            mmltk::live::device_ptr_as_bytes(slot->device_ptr),
            slot->pitch_bytes,
            static_cast<int>(snapshot.width),
            static_cast<int>(snapshot.height),
            reinterpret_cast<const int*>(mmltk::live::device_ptr_as_void(device_points_upload_.data())),
            static_cast<int>(marker_set.points.size()),
            std::max(1, marker_set.radius),
            marker_set.r,
            marker_set.g,
            marker_set.b,
            marker_set.alpha,
            slot->stream);
        ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_points_rgba_pitched for interaction overlay");
    }

    ensure_cuda_ok(cudaEventRecord(slot->ready_event, slot->stream),
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
