#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <functional>

#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/system_image_worker.h"

namespace mmltk::frameworks::gpu::test_support {

inline void CopyImagePlane(const ImagePlaneView destination, const ImagePlaneView source) {
    for (std::uint32_t row = 0U; row != source.descriptor.height; ++row) {
        std::memcpy(reinterpret_cast<std::byte*>(destination.data) + row * destination.descriptor.pitch_bytes,
                    reinterpret_cast<const std::byte*>(source.data) + row * source.descriptor.pitch_bytes, source.descriptor.row_bytes());
    }
}

class FakeImageBackend final : public ImageCopyBackend {
   public:
    [[nodiscard]] std::optional<DeviceExecution> ResolveExecution(int, int) override { return {}; }
    enum class FailurePoint : std::uint8_t {
        None,
        CreateContext,
        Bind,
        CreateStream,
        AllocatePlane,
        Copy,
        RecordEvent,
        SynchronizeEvent,
        SynchronizeStream,
        // CLEANUP-IGNORE: The fake backend's failure inventory is independent from its deliberately granular counters.
    };

    std::atomic<std::size_t> contexts_created{0U};
    std::atomic<std::size_t> contexts_destroyed{0U};
    std::atomic<std::size_t> contexts_bound{0U};
    std::atomic<std::uintptr_t> last_bound_context{0U};
    std::atomic<std::size_t> streams_destroyed{0U};
    std::atomic<std::size_t> planes_allocated{0U};
    std::atomic<std::size_t> planes_freed{0U};
    std::atomic<std::size_t> pinned_allocated{0U};
    std::atomic<int> pinned_receiver_device{-1};
    std::atomic<std::size_t> synchronized{0U};
    std::atomic<std::size_t> same_copies{0U};
    std::atomic<std::size_t> peer_copies{0U};
    std::atomic<std::size_t> staged_downloads{0U};
    std::atomic<std::size_t> staged_uploads{0U};
    bool peer_access = false;
    bool defer_events = false;
    std::atomic_bool defer_same_device_copies{false};

    void FailAfter(const FailurePoint point, const std::size_t successful_calls = 0U) noexcept {
        failure_point_ = point;
        successful_calls_before_failure_ = successful_calls;
        persistent_failure_ = false;
    }
    void FailPersistently(const FailurePoint point) noexcept {
        failure_point_ = point;
        successful_calls_before_failure_ = 0U;
        persistent_failure_ = true;
    }

    void CompleteEvents() noexcept {
        {
            std::scoped_lock lock(mutex_);
            for (auto& event : events_)
                event.second = true;
        }
        event_ready_.notify_all();
    }
    [[nodiscard]] bool WaitForSameDeviceCopy(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(copy_mutex_);
        return copy_changed_.wait_for(lock, timeout, [this] { return same_device_copies_entered_ != 0U; });
    }
    void CompleteSameDeviceCopies() noexcept {
        defer_same_device_copies.store(false, std::memory_order_release);
        copy_changed_.notify_all();
    }

    [[nodiscard]] std::uintptr_t CreateContext(const int device, DeviceContextMode) override {
        MaybeFail(FailurePoint::CreateContext);
        const std::uintptr_t context = next_.fetch_add(1U);
        {
            std::scoped_lock lock(mutex_);
            devices_[context] = device;
        }
        ++contexts_created;
        return context;
    }
    void DestroyContext(const int, DeviceContextMode, const std::uintptr_t context) noexcept override {
        std::scoped_lock lock(mutex_);
        devices_.erase(context);
        ++contexts_destroyed;
    }
    void BindContext(const std::uintptr_t context) override {
        MaybeFail(FailurePoint::Bind);
        last_bound_context.store(context, std::memory_order_release);
        ++contexts_bound;
    }
    [[nodiscard]] std::uintptr_t CreateStream(std::uintptr_t) override {
        MaybeFail(FailurePoint::CreateStream);
        return next_.fetch_add(1U);
    }
    void DestroyStream(std::uintptr_t, std::uintptr_t) noexcept override { ++streams_destroyed; }
    [[nodiscard]] std::uintptr_t CreateEvent(std::uintptr_t) override {
        const auto event = next_.fetch_add(1U);
        std::scoped_lock lock(mutex_);
        events_[event] = !defer_events;
        return event;
    }
    void DestroyEvent(std::uintptr_t, const std::uintptr_t event) noexcept override {
        std::scoped_lock lock(mutex_);
        events_.erase(event);
    }
    [[nodiscard]] ImagePlaneView AllocatePlane(std::uintptr_t, const ImagePlaneKind kind, const std::uint32_t width,
                                               const std::uint32_t height) override {
        MaybeFail(FailurePoint::AllocatePlane);
        const std::size_t pitch = static_cast<std::size_t>(width) * 4U;
        auto* storage = new std::byte[pitch * height];
        ++planes_allocated;
        return {
            .data = reinterpret_cast<CUdeviceptr>(storage),
            .descriptor =
                {
                    .kind = kind,
                    .format = ImageFormat::Rgba8,
                    .width = width,
                    .height = height,
                    .pitch_bytes = pitch,
                },
        };
    }
    void FreePlane(std::uintptr_t, const CUdeviceptr data) noexcept override {
        delete[] reinterpret_cast<std::byte*>(data);
        ++planes_freed;
    }
    [[nodiscard]] std::shared_ptr<void> AllocatePinned(std::uintptr_t context, const mmltk::common::system::ExecutionPlacement*,
                                                       const std::size_t bytes) override {
        ++pinned_allocated;
        {
            std::scoped_lock lock(mutex_);
            pinned_receiver_device = devices_.at(context);
        }
        return {new std::byte[bytes], [](void* data) { delete[] static_cast<std::byte*>(data); }};
    }
    [[nodiscard]] bool CanAccessPeer(int, int) override { return peer_access; }
    void WaitEvent(std::uintptr_t, std::uintptr_t, const std::uintptr_t event) override { Wait(event); }
    void CopySameDevice(std::uintptr_t, std::uintptr_t, const ImagePlaneView& destination, std::uintptr_t,
                        const ImagePlaneView& source) override {
        MaybeFail(FailurePoint::Copy);
        {
            std::unique_lock lock(copy_mutex_);
            ++same_device_copies_entered_;
            copy_changed_.notify_all();
            copy_changed_.wait(lock, [this] { return !defer_same_device_copies.load(std::memory_order_acquire); });
        }
        CopyImagePlane(destination, source);
        ++same_copies;
    }
    void CopyPeer(std::uintptr_t, std::uintptr_t, int, const ImagePlaneView& destination, std::uintptr_t, int,
                  const ImagePlaneView& source) override {
        MaybeFail(FailurePoint::Copy);
        CopyImagePlane(destination, source);
        ++peer_copies;
    }
    void CopyDeviceToHost(std::uintptr_t, const ImagePlaneView& source, void* const destination,
                          const std::size_t destination_pitch) override {
        MaybeFail(FailurePoint::Copy);
        for (std::uint32_t row = 0U; row != source.descriptor.height; ++row) {
            std::memcpy(static_cast<std::byte*>(destination) + row * destination_pitch,
                        reinterpret_cast<const std::byte*>(source.data) + row * source.descriptor.pitch_bytes,
                        source.descriptor.row_bytes());
        }
        ++staged_downloads;
    }
    void CopyHostToDevice(std::uintptr_t, std::uintptr_t, const void* const source, const std::size_t source_pitch,
                          const ImagePlaneView& destination) override {
        MaybeFail(FailurePoint::Copy);
        for (std::uint32_t row = 0U; row != destination.descriptor.height; ++row) {
            std::memcpy(reinterpret_cast<std::byte*>(destination.data) + row * destination.descriptor.pitch_bytes,
                        static_cast<const std::byte*>(source) + row * source_pitch, destination.descriptor.row_bytes());
        }
        ++staged_uploads;
    }
    void RecordEvent(std::uintptr_t, std::uintptr_t, const std::uintptr_t event) override {
        MaybeFail(FailurePoint::RecordEvent);
        std::scoped_lock lock(mutex_);
        events_[event] = !defer_events;
    }
    void SynchronizeEvent(std::uintptr_t, const std::uintptr_t event) override {
        MaybeFail(FailurePoint::SynchronizeEvent);
        Wait(event);
    }
    StreamSettlement SettleStream(const std::uintptr_t context, std::uintptr_t) noexcept override {
        try {
            MaybeFail(FailurePoint::Bind);
            last_bound_context.store(context, std::memory_order_release);
            ++contexts_bound;
        } catch (...) { return {.failure = std::current_exception()}; }
        try {
            MaybeFail(FailurePoint::SynchronizeStream);
        } catch (...) {
            ++synchronized;
            return {.completion_reached = true, .failure = std::current_exception()};
        }
        ++synchronized;
        return {.completion_reached = true};
    }

   private:
    void MaybeFail(const FailurePoint point) {
        if (failure_point_ != point) return;
        if (successful_calls_before_failure_ != 0U) {
            --successful_calls_before_failure_;
            return;
        }
        if (!persistent_failure_) failure_point_ = FailurePoint::None;
        throw std::runtime_error("injected image backend failure");
    }

    void Wait(const std::uintptr_t event) {
        std::unique_lock lock(mutex_);
        event_ready_.wait(lock, [this, event] {
            const auto found = events_.find(event);
            return found == events_.end() || found->second;
        });
    }

    std::atomic<std::uintptr_t> next_{1U};
    std::mutex mutex_;
    std::condition_variable event_ready_;
    std::mutex copy_mutex_;
    std::condition_variable copy_changed_;
    std::size_t same_device_copies_entered_ = 0U;
    std::unordered_map<std::uintptr_t, int> devices_;
    std::unordered_map<std::uintptr_t, bool> events_;
    FailurePoint failure_point_ = FailurePoint::None;
    std::size_t successful_calls_before_failure_ = 0U;
    bool persistent_failure_ = false;
};

[[nodiscard]] inline std::function<std::unique_ptr<SystemImageRuntime>()> RuntimeFactory(
    const int device, const std::shared_ptr<FakeImageBackend>& backend, const ImageProductLayout layout = ImageProductLayout::Clean,
    std::function<std::unique_ptr<SystemImageModel>()> model = {}) {
    return [device, backend, layout, model = std::move(model)] {
        return std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{
            .device = device,
            .backend = backend,
            .model = model ? model() : nullptr,
            .input_layout = layout,
            .output_layout = layout,
        });
    };
}

}  // namespace mmltk::frameworks::gpu::test_support
