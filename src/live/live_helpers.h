#pragma once

#include "mmltk/live/live_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mmltk::live {

inline void ensure_cuda_ok(const cudaError_t status, const char* context) {
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(context) + ": " + cudaGetErrorString(status));
    }
}

inline bool event_ready(const cudaEvent_t event,
                        const char* context = "cudaEventQuery") {
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

template <typename T>
class PinnedUploadBuffer {
public:
    PinnedUploadBuffer() = default;
    ~PinnedUploadBuffer() { reset(); }

    PinnedUploadBuffer(const PinnedUploadBuffer&) = delete;
    PinnedUploadBuffer& operator=(const PinnedUploadBuffer&) = delete;

    PinnedUploadBuffer(PinnedUploadBuffer&& other) noexcept
        : data_(other.data_),
          capacity_(other.capacity_) {
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
        ensure_cuda_ok(cudaHostAlloc(reinterpret_cast<void**>(&data_),
                                     value_count * sizeof(T),
                                     cudaHostAllocPortable),
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

    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return data_ == nullptr; }

private:
    T* data_ = nullptr;
    std::size_t capacity_ = 0;
};

template <typename T>
class DeviceUploadBuffer {
public:
    DeviceUploadBuffer() = default;
    ~DeviceUploadBuffer() { reset(); }

    DeviceUploadBuffer(const DeviceUploadBuffer&) = delete;
    DeviceUploadBuffer& operator=(const DeviceUploadBuffer&) = delete;

    DeviceUploadBuffer(DeviceUploadBuffer&& other) noexcept
        : device_ptr_(other.device_ptr_),
          capacity_(other.capacity_) {
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
        ensure_cuda_ok(cudaMalloc(reinterpret_cast<void**>(&device_ptr_), value_count * sizeof(T)),
                       context);
        capacity_ = value_count;
    }

    void reset() noexcept {
        if (device_ptr_ != 0) {
            (void)cudaFree(device_ptr_as_void(device_ptr_));
            device_ptr_ = 0;
        }
        capacity_ = 0;
    }

    [[nodiscard]] CUdeviceptr data() const noexcept { return device_ptr_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return device_ptr_ == 0; }

private:
    CUdeviceptr device_ptr_ = 0;
    std::size_t capacity_ = 0;
};

template <typename T = std::uint8_t>
class PitchedDeviceBuffer {
public:
    PitchedDeviceBuffer() = default;
    ~PitchedDeviceBuffer() { reset(); }

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
        ensure_cuda_ok(cudaMallocPitch(reinterpret_cast<void**>(&device_ptr_),
                                       &pitch_bytes_,
                                       static_cast<std::size_t>(width) * sizeof(T),
                                       height),
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

    [[nodiscard]] CUdeviceptr data() const noexcept { return device_ptr_; }
    [[nodiscard]] std::size_t pitch_bytes() const noexcept { return pitch_bytes_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] bool empty() const noexcept { return device_ptr_ == 0; }

private:
    CUdeviceptr device_ptr_ = 0;
    std::size_t pitch_bytes_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

class CudaStreamHandle {
public:
    CudaStreamHandle() = default;
    ~CudaStreamHandle() { reset(); }

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

    [[nodiscard]] cudaStream_t get() const noexcept { return stream_; }
    [[nodiscard]] cudaStream_t release() noexcept {
        cudaStream_t stream = stream_;
        stream_ = nullptr;
        return stream;
    }
    [[nodiscard]] bool empty() const noexcept { return stream_ == nullptr; }

private:
    cudaStream_t stream_ = nullptr;
};

class CudaEventHandle {
public:
    CudaEventHandle() = default;
    ~CudaEventHandle() { reset(); }

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

    [[nodiscard]] cudaEvent_t get() const noexcept { return event_; }
    [[nodiscard]] cudaEvent_t release() noexcept {
        cudaEvent_t event = event_;
        event_ = nullptr;
        return event;
    }
    [[nodiscard]] bool empty() const noexcept { return event_ == nullptr; }

private:
    cudaEvent_t event_ = nullptr;
};

template <typename Slot, typename Output, typename BuildOutput>
bool try_acquire_published_slot(Slot& slot, Output* out, BuildOutput&& build_output) {
    std::uint32_t expected = to_slot_state_value(SlotState::kPublished);
    if (!slot.state.compare_exchange_strong(expected,
                                            to_slot_state_value(SlotState::kAcquired),
                                            std::memory_order_acq_rel)) {
        return false;
    }

    *out = build_output(slot);
    return true;
}

template <typename Slot>
void release_acquired_slot(Slot& slot, const char* context) {
    std::uint32_t expected = to_slot_state_value(SlotState::kAcquired);
    if (!slot.state.compare_exchange_strong(expected,
                                            to_slot_state_value(SlotState::kFree),
                                            std::memory_order_acq_rel)) {
        throw std::runtime_error(context);
    }
}

template <typename Slot, typename Output, typename BuildOutput>
bool try_acquire_latest_published_slot(std::vector<std::unique_ptr<Slot>>& slots,
                                       const std::atomic<int>& latest_index,
                                       Output* out,
                                       BuildOutput&& build_output) {
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

template <typename Slot, typename ResetSlot, typename ReadyEventAccessor>
Slot* reserve_writable_slot(std::vector<std::unique_ptr<Slot>>& slots,
                            std::atomic<int>& latest_index,
                            ResetSlot&& reset_slot,
                            ReadyEventAccessor&& ready_event_of,
                            const char* event_context) {
    for (auto& slot : slots) {
        std::uint32_t expected = to_slot_state_value(SlotState::kFree);
        if (slot->state.compare_exchange_strong(expected,
                                                to_slot_state_value(SlotState::kWriting),
                                                std::memory_order_acq_rel)) {
            return slot.get();
        }

        if (expected != to_slot_state_value(SlotState::kPublished) ||
            !event_ready(ready_event_of(*slot), event_context)) {
            continue;
        }

        expected = to_slot_state_value(SlotState::kPublished);
        if (slot->state.compare_exchange_strong(expected,
                                                to_slot_state_value(SlotState::kWriting),
                                                std::memory_order_acq_rel)) {
            if (latest_index.load(std::memory_order_acquire) == static_cast<int>(slot->slot_index)) {
                latest_index.store(-1, std::memory_order_release);
            }
            reset_slot(*slot);
            return slot.get();
        }
    }
    return nullptr;
}

inline void store_error_message(std::shared_ptr<const std::string>* target, std::string error_message) {
    if (target == nullptr) {
        return;
    }
    auto next = std::make_shared<std::string>(std::move(error_message));
    std::shared_ptr<const std::string> immutable = std::move(next);
    std::atomic_store_explicit(target, std::move(immutable), std::memory_order_release);
}

} // namespace mmltk::live
