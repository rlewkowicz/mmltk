#pragma once

#include "mmltk/live/live_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cudaEGL.h>

struct gbm_bo;
struct gbm_device;

namespace mmltk::live {

inline void ensure_cuda_ok(const cudaError_t status, const char* context) {
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(context) + ": " + cudaGetErrorString(status));
    }
}

void ensure_cuda_driver_ok(CUresult status, const char* context);

inline bool event_ready(const cudaEvent_t event, const char* context = "cudaEventQuery") {
    if (event == nullptr) {
        return true;
    }
    const cudaError_t status = cudaEventQuery(event);
    if (status == cudaErrorNotReady) {
        return false;
    }
    ensure_cuda_ok(status, context);
    return true;
}

struct Bgr24Pixel {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
};

static_assert(sizeof(Bgr24Pixel) == 3U, "Bgr24Pixel must stay tightly packed");

struct Rgba32Pixel {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
};

static_assert(sizeof(Rgba32Pixel) == 4U, "Rgba32Pixel must stay tightly packed");

template <typename T>
class PinnedUploadBuffer {
   public:
    PinnedUploadBuffer() = default;
    ~PinnedUploadBuffer() {
        reset();
    }

    PinnedUploadBuffer(const PinnedUploadBuffer&) = delete;
    PinnedUploadBuffer& operator=(const PinnedUploadBuffer&) = delete;

    PinnedUploadBuffer(PinnedUploadBuffer&& other) noexcept : data_(other.data_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.capacity_ = 0;
    }

    PinnedUploadBuffer& operator=(PinnedUploadBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = other.data_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.capacity_ = 0;
        }
        return *this;
    }

    void ensure_capacity(std::size_t value_count, const char* context) {
        if (value_count == 0U) {
            return;
        }
        if (capacity_ >= value_count && data_ != nullptr) {
            return;
        }
        if (data_ != nullptr) {
            ensure_cuda_ok(cudaFreeHost(data_), context);
        }
        ensure_cuda_ok(cudaHostAlloc(reinterpret_cast<void**>(&data_), value_count * sizeof(T), cudaHostAllocPortable),
                       context);
        capacity_ = value_count;
    }

    void reset() noexcept {
        if (data_ != nullptr) {
            (void)cudaFreeHost(data_);
            data_ = nullptr;
        }
        capacity_ = 0;
    }

    [[nodiscard]] T* data() noexcept {
        return data_;
    }
    [[nodiscard]] const T* data() const noexcept {
        return data_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return data_ == nullptr;
    }

   private:
    T* data_ = nullptr;
    std::size_t capacity_ = 0;
};

template <typename T>
class DeviceUploadBuffer {
   public:
    DeviceUploadBuffer() = default;
    ~DeviceUploadBuffer() {
        reset();
    }

    DeviceUploadBuffer(const DeviceUploadBuffer&) = delete;
    DeviceUploadBuffer& operator=(const DeviceUploadBuffer&) = delete;

    DeviceUploadBuffer(DeviceUploadBuffer&& other) noexcept
        : device_ptr_(other.device_ptr_), capacity_(other.capacity_) {
        other.device_ptr_ = 0;
        other.capacity_ = 0;
    }

    DeviceUploadBuffer& operator=(DeviceUploadBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            device_ptr_ = other.device_ptr_;
            capacity_ = other.capacity_;
            other.device_ptr_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    void ensure_capacity(std::size_t value_count, const char* context) {
        if (value_count == 0U) {
            return;
        }
        if (capacity_ >= value_count && device_ptr_ != 0) {
            return;
        }
        if (device_ptr_ != 0) {
            ensure_cuda_ok(cudaFree(device_ptr_as_void(device_ptr_)), context);
        }
        ensure_cuda_ok(cudaMalloc(reinterpret_cast<void**>(&device_ptr_), value_count * sizeof(T)), context);
        capacity_ = value_count;
    }

    void reset() noexcept {
        if (device_ptr_ != 0) {
            (void)cudaFree(device_ptr_as_void(device_ptr_));
            device_ptr_ = 0;
        }
        capacity_ = 0;
    }

    [[nodiscard]] CUdeviceptr data() const noexcept {
        return device_ptr_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return device_ptr_ == 0;
    }

   private:
    CUdeviceptr device_ptr_ = 0;
    std::size_t capacity_ = 0;
};

template <typename T = std::uint8_t>
class PitchedDeviceBuffer {
   public:
    PitchedDeviceBuffer() = default;
    ~PitchedDeviceBuffer() {
        reset();
    }

    PitchedDeviceBuffer(const PitchedDeviceBuffer&) = delete;
    PitchedDeviceBuffer& operator=(const PitchedDeviceBuffer&) = delete;

    PitchedDeviceBuffer(PitchedDeviceBuffer&& other) noexcept
        : device_ptr_(other.device_ptr_),
          pitch_bytes_(other.pitch_bytes_),
          width_(other.width_),
          height_(other.height_) {
        other.device_ptr_ = 0;
        other.pitch_bytes_ = 0;
        other.width_ = 0;
        other.height_ = 0;
    }

    PitchedDeviceBuffer& operator=(PitchedDeviceBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            device_ptr_ = other.device_ptr_;
            pitch_bytes_ = other.pitch_bytes_;
            width_ = other.width_;
            height_ = other.height_;
            other.device_ptr_ = 0;
            other.pitch_bytes_ = 0;
            other.width_ = 0;
            other.height_ = 0;
        }
        return *this;
    }

    void ensure_dimensions(std::uint32_t width, std::uint32_t height, const char* context) {
        if (width == 0U || height == 0U) {
            return;
        }
        if (device_ptr_ != 0 && width_ >= width && height_ >= height) {
            return;
        }
        reset();
        ensure_cuda_ok(cudaMallocPitch(reinterpret_cast<void**>(&device_ptr_), &pitch_bytes_,
                                       static_cast<std::size_t>(width) * sizeof(T), height),
                       context);
        width_ = width;
        height_ = height;
    }

    void reset() noexcept {
        if (device_ptr_ != 0) {
            (void)cudaFree(device_ptr_as_void(device_ptr_));
            device_ptr_ = 0;
        }
        pitch_bytes_ = 0;
        width_ = 0;
        height_ = 0;
    }

    [[nodiscard]] CUdeviceptr data() const noexcept {
        return device_ptr_;
    }
    [[nodiscard]] std::size_t pitch_bytes() const noexcept {
        return pitch_bytes_;
    }
    [[nodiscard]] std::uint32_t width() const noexcept {
        return width_;
    }
    [[nodiscard]] std::uint32_t height() const noexcept {
        return height_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return device_ptr_ == 0;
    }

   private:
    CUdeviceptr device_ptr_ = 0;
    std::size_t pitch_bytes_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

using BgrPitchedDeviceBuffer = PitchedDeviceBuffer<Bgr24Pixel>;
using RgbaPitchedDeviceBuffer = PitchedDeviceBuffer<Rgba32Pixel>;

struct GpuInteropAllocationProfile {
    enum class Mode : std::uint8_t {
        ExplicitModifier = 0,
        ImplicitModifier = 1,
    };

    Mode mode = Mode::ImplicitModifier;
    std::uint32_t usage_flags = 0;
    std::uint64_t modifier = kWorkspaceDmaBufDrmFormatModInvalid;
    DmaBufModifierMode modifier_mode = DmaBufModifierMode::Unknown;
    const char* label = "";
};

class LinuxGpuInteropDevice {
   public:
    LinuxGpuInteropDevice() = default;
    ~LinuxGpuInteropDevice();

    LinuxGpuInteropDevice(const LinuxGpuInteropDevice&) = delete;
    LinuxGpuInteropDevice& operator=(const LinuxGpuInteropDevice&) = delete;

    LinuxGpuInteropDevice(LinuxGpuInteropDevice&& other) noexcept;
    LinuxGpuInteropDevice& operator=(LinuxGpuInteropDevice&& other) noexcept;

    void ensure_open(int cuda_device_index, const char* context);
    void reset() noexcept;

    [[nodiscard]] gbm_device* get() const noexcept {
        return device_;
    }
    [[nodiscard]] int fd() const noexcept {
        return fd_;
    }
    [[nodiscard]] int cuda_device_index() const noexcept {
        return cuda_device_index_;
    }
    [[nodiscard]] const std::string& render_node_path() const noexcept {
        return render_node_path_;
    }
    [[nodiscard]] const std::string& gbm_backend_name() const noexcept {
        return gbm_backend_name_;
    }
    [[nodiscard]] EGLDisplay egl_display() const noexcept {
        return egl_display_;
    }
    [[nodiscard]] PFNEGLCREATEIMAGEKHRPROC egl_create_image() const noexcept {
        return egl_create_image_;
    }
    [[nodiscard]] PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image() const noexcept {
        return egl_destroy_image_;
    }
    [[nodiscard]] const std::vector<std::uint64_t>& dma_buf_modifiers() const noexcept {
        return dma_buf_modifiers_;
    }
    [[nodiscard]] bool has_dma_buf_import_modifiers() const noexcept {
        return has_dma_buf_import_modifiers_;
    }
    [[nodiscard]] bool omit_modifier_attrs_for_implicit_imports() const noexcept {
        return omit_modifier_attrs_for_implicit_imports_;
    }
    [[nodiscard]] const std::optional<GpuInteropAllocationProfile>& cached_allocation_profile() const noexcept {
        return cached_allocation_profile_;
    }
    void cache_allocation_profile(GpuInteropAllocationProfile profile) {
        cached_allocation_profile_ = profile;
    }
    [[nodiscard]] bool empty() const noexcept {
        return device_ == nullptr;
    }

   private:
    void clear_after_move() noexcept;

    int fd_ = -1;
    int cuda_device_index_ = -1;
    std::string render_node_path_;
    std::string gbm_backend_name_;
    gbm_device* device_ = nullptr;
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    PFNEGLCREATEIMAGEKHRPROC egl_create_image_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image_ = nullptr;
    PFNEGLQUERYDMABUFFORMATSEXTPROC egl_query_dma_buf_formats_ = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC egl_query_dma_buf_modifiers_ = nullptr;
    std::vector<std::uint64_t> dma_buf_modifiers_;
    bool has_dma_buf_import_modifiers_ = false;
    bool omit_modifier_attrs_for_implicit_imports_ = true;
    std::optional<GpuInteropAllocationProfile> cached_allocation_profile_;
};

class DmaBufCudaRgbaSurface {
   public:
    DmaBufCudaRgbaSurface() = default;
    ~DmaBufCudaRgbaSurface();

    DmaBufCudaRgbaSurface(const DmaBufCudaRgbaSurface&) = delete;
    DmaBufCudaRgbaSurface& operator=(const DmaBufCudaRgbaSurface&) = delete;

    DmaBufCudaRgbaSurface(DmaBufCudaRgbaSurface&& other) noexcept;
    DmaBufCudaRgbaSurface& operator=(DmaBufCudaRgbaSurface&& other) noexcept;

    void ensure_dimensions(LinuxGpuInteropDevice& interop_device, int cuda_device_index, std::uint32_t width,
                           std::uint32_t height, const char* context);
    void reset() noexcept;

    [[nodiscard]] CUdeviceptr data() const noexcept {
        return device_ptr_;
    }
    [[nodiscard]] std::size_t pitch_bytes() const noexcept {
        return stride_bytes_;
    }
    [[nodiscard]] std::uint32_t width() const noexcept {
        return width_;
    }
    [[nodiscard]] std::uint32_t height() const noexcept {
        return height_;
    }
    [[nodiscard]] int fd() const noexcept {
        return fd_;
    }
    [[nodiscard]] int duplicate_fd(const char* context) const;
    [[nodiscard]] WorkspaceDmaBufImage dmabuf_image(std::uint32_t published_width,
                                                    std::uint32_t published_height) const;
    [[nodiscard]] bool empty() const noexcept {
        return cuda_resource_ == nullptr;
    }
    [[nodiscard]] bool is_pitch_frame() const noexcept {
        return frame_type_ == CU_EGL_FRAME_TYPE_PITCH && device_ptr_ != 0;
    }
    [[nodiscard]] bool is_array_frame() const noexcept {
        return frame_type_ == CU_EGL_FRAME_TYPE_ARRAY && surface_object_ != 0;
    }
    [[nodiscard]] cudaSurfaceObject_t surface_object() const noexcept {
        return static_cast<cudaSurfaceObject_t>(surface_object_);
    }
    [[nodiscard]] CUarray array() const noexcept {
        return array_;
    }
    [[nodiscard]] std::uint32_t channel_count() const noexcept {
        return channel_count_;
    }
    [[nodiscard]] CUarray_format cuda_format() const noexcept {
        return cuda_format_;
    }
    [[nodiscard]] CUeglFrameType frame_type() const noexcept {
        return frame_type_;
    }
    [[nodiscard]] DmaBufModifierMode modifier_mode() const noexcept {
        return modifier_mode_;
    }

   private:
    void initialize(LinuxGpuInteropDevice& interop_device, const GpuInteropAllocationProfile& profile,
                    std::uint32_t width, std::uint32_t height, const char* context);
    void clear_after_move() noexcept;

    gbm_bo* bo_ = nullptr;
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image_ = nullptr;
    EGLImageKHR egl_image_ = EGL_NO_IMAGE_KHR;
    CUgraphicsResource cuda_resource_ = nullptr;
    CUsurfObject surface_object_ = 0;
    CUarray array_ = nullptr;
    CUdeviceptr device_ptr_ = 0;
    int fd_ = -1;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::size_t stride_bytes_ = 0;
    std::uint64_t offset_ = 0;
    std::uint64_t allocation_size_ = 0;
    std::uint32_t drm_format_ = 0;
    std::uint64_t drm_modifier_ = 0;
    DmaBufModifierMode modifier_mode_ = DmaBufModifierMode::Unknown;
    CUeglFrameType frame_type_ = CU_EGL_FRAME_TYPE_PITCH;
    std::uint32_t channel_count_ = 0;
    CUarray_format cuda_format_ = CU_AD_FORMAT_UNSIGNED_INT8;
};

class CudaStreamHandle {
   public:
    CudaStreamHandle() = default;
    ~CudaStreamHandle() {
        reset();
    }

    CudaStreamHandle(const CudaStreamHandle&) = delete;
    CudaStreamHandle& operator=(const CudaStreamHandle&) = delete;

    CudaStreamHandle(CudaStreamHandle&& other) noexcept : stream_(other.release()) {}

    CudaStreamHandle& operator=(CudaStreamHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    void create_with_highest_priority(const char* context, unsigned int flags = cudaStreamNonBlocking) {
        reset();
        int least_priority = 0;
        int greatest_priority = 0;
        ensure_cuda_ok(cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority), context);
        ensure_cuda_ok(cudaStreamCreateWithPriority(&stream_, flags, greatest_priority), context);
    }

    void reset(cudaStream_t stream = nullptr) noexcept {
        if (stream_ != nullptr) {
            (void)cudaStreamDestroy(stream_);
        }
        stream_ = stream;
    }

    [[nodiscard]] cudaStream_t get() const noexcept {
        return stream_;
    }
    [[nodiscard]] cudaStream_t release() noexcept {
        cudaStream_t stream = stream_;
        stream_ = nullptr;
        return stream;
    }
    [[nodiscard]] bool empty() const noexcept {
        return stream_ == nullptr;
    }

   private:
    cudaStream_t stream_ = nullptr;
};

class CudaEventHandle {
   public:
    CudaEventHandle() = default;
    ~CudaEventHandle() {
        reset();
    }

    CudaEventHandle(const CudaEventHandle&) = delete;
    CudaEventHandle& operator=(const CudaEventHandle&) = delete;

    CudaEventHandle(CudaEventHandle&& other) noexcept : event_(other.release()) {}

    CudaEventHandle& operator=(CudaEventHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    void create(unsigned int flags, const char* context) {
        reset();
        ensure_cuda_ok(cudaEventCreateWithFlags(&event_, flags), context);
    }

    void reset(cudaEvent_t event = nullptr) noexcept {
        if (event_ != nullptr) {
            (void)cudaEventDestroy(event_);
        }
        event_ = event;
    }

    [[nodiscard]] cudaEvent_t get() const noexcept {
        return event_;
    }
    [[nodiscard]] cudaEvent_t release() noexcept {
        cudaEvent_t event = event_;
        event_ = nullptr;
        return event;
    }
    [[nodiscard]] bool empty() const noexcept {
        return event_ == nullptr;
    }

   private:
    cudaEvent_t event_ = nullptr;
};

template <typename Slot, typename Output, typename BuildOutput>
bool try_acquire_published_slot(Slot& slot, Output* out, BuildOutput&& build_output) {
    std::uint32_t expected = to_slot_state_value(SlotState::kPublished);
    if (!slot.state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kAcquired),
                                            std::memory_order_acq_rel)) {
        return false;
    }

    *out = build_output(slot);
    return true;
}

template <typename Slot, typename Output, typename BuildOutput, typename ReadyEventAccessor>
bool try_acquire_ready_published_slot(Slot& slot, Output* out, BuildOutput&& build_output,
                                      ReadyEventAccessor&& ready_event_of, const char* event_context) {
    if (slot.state.load(std::memory_order_acquire) != to_slot_state_value(SlotState::kPublished)) {
        return false;
    }
    if (!event_ready(ready_event_of(slot), event_context)) {
        return false;
    }

    std::uint32_t expected = to_slot_state_value(SlotState::kPublished);
    if (!slot.state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kAcquired),
                                            std::memory_order_acq_rel)) {
        return false;
    }

    *out = build_output(slot);
    return true;
}

template <typename Slot>
void release_acquired_slot(Slot& slot, const char* context) {
    std::uint32_t expected = to_slot_state_value(SlotState::kAcquired);
    if (!slot.state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kFree),
                                            std::memory_order_acq_rel)) {
        throw std::runtime_error(context);
    }
}

template <typename Slot, typename Output, typename BuildOutput>
bool try_acquire_latest_published_slot(std::vector<std::unique_ptr<Slot>>& slots, const std::atomic<int>& latest_index,
                                       Output* out, BuildOutput&& build_output) {
    const int latest = latest_index.load(std::memory_order_acquire);
    if (latest >= 0 && latest < static_cast<int>(slots.size()) &&
        try_acquire_published_slot(*slots[static_cast<std::size_t>(latest)], out, build_output)) {
        return true;
    }

    for (auto& slot : slots) {
        if (static_cast<int>(slot->slot_index) == latest) {
            continue;
        }
        if (try_acquire_published_slot(*slot, out, build_output)) {
            return true;
        }
    }
    return false;
}

template <typename Slot, typename Output, typename BuildOutput, typename ReadyEventAccessor>
bool try_acquire_latest_ready_published_slot(std::vector<std::unique_ptr<Slot>>& slots,
                                             const std::atomic<int>& latest_index, Output* out,
                                             BuildOutput&& build_output, ReadyEventAccessor&& ready_event_of,
                                             const char* event_context) {
    const int latest = latest_index.load(std::memory_order_acquire);
    if (latest >= 0 && latest < static_cast<int>(slots.size()) &&
        try_acquire_ready_published_slot(*slots[static_cast<std::size_t>(latest)], out, build_output, ready_event_of,
                                         event_context)) {
        return true;
    }

    for (auto& slot : slots) {
        if (static_cast<int>(slot->slot_index) == latest) {
            continue;
        }
        if (try_acquire_ready_published_slot(*slot, out, build_output, ready_event_of, event_context)) {
            return true;
        }
    }
    return false;
}

template <typename Slot, typename ResetSlot, typename ReadyEventAccessor>
Slot* reserve_writable_slot(std::vector<std::unique_ptr<Slot>>& slots, std::atomic<int>& latest_index,
                            ResetSlot&& reset_slot, ReadyEventAccessor&& ready_event_of, const char* event_context) {
    for (auto& slot : slots) {
        std::uint32_t expected = to_slot_state_value(SlotState::kFree);
        if (slot->state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kWriting),
                                                std::memory_order_acq_rel)) {
            return slot.get();
        }

        if (expected != to_slot_state_value(SlotState::kPublished) ||
            !event_ready(ready_event_of(*slot), event_context)) {
            continue;
        }
        if (latest_index.load(std::memory_order_acquire) == static_cast<int>(slot->slot_index)) {
            continue;
        }

        expected = to_slot_state_value(SlotState::kPublished);
        if (slot->state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kWriting),
                                                std::memory_order_acq_rel)) {
            reset_slot(*slot);
            return slot.get();
        }
    }
    return nullptr;
}

inline void store_error_message(std::atomic<std::shared_ptr<const std::string>>* target, std::string error_message) {
    if (target == nullptr) {
        return;
    }
    auto next = std::make_shared<std::string>(std::move(error_message));
    std::shared_ptr<const std::string> immutable = std::move(next);
    target->store(std::move(immutable), std::memory_order_release);
}

}  // namespace mmltk::live
