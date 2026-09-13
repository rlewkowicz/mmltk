#pragma once

#include "src/acceptance/tests/async_test_utils.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <functional>
#include <future>
#include <algorithm>
#include <utility>
#include <vector>

#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/frameworks/gpu/imported_image_buffer.h"
#include "src/frameworks/gpu/system_image_runtime.h"

namespace mmltk::frameworks::gpu::test_support {

// The fixture records the physical mapped-base release before external-memory
// destruction. A failed mapping release must retain backing and context.
struct ImportedImageBufferTestAccess final {
    static inline CUresult unmap_result = CUDA_SUCCESS;
    static inline CUresult allocation_result = CUDA_SUCCESS;
    static inline std::size_t unmaps = 0U;
    static inline std::size_t allocation_releases = 0U;
    static inline CUdeviceptr last_freed_base = 0U;
    static inline std::unordered_map<CUdeviceptr, std::size_t> workspace_storage;
    static void Reset() noexcept {
        unmap_result = allocation_result = CUDA_SUCCESS;
        unmaps = allocation_releases = 0U;
        last_freed_base = 0U;
    }
    static void Adopt(ImportedImageBuffer& buffer, mmltk::common::io::ScopedFd backing = {}) noexcept {
        auto& resource = *buffer.resources_;
        resource.backing = std::move(backing);
        resource.memory = reinterpret_cast<CUexternalMemory>(11U);
        resource.mapped_base = 22U;
        resource.data = 150U;
        resource.layout.required_allocation_bytes = 4096U;
        resource.layout.pitch_bytes = 64U;
    }
    static cudaError_t Release(ImportedImageBuffer& buffer) noexcept { return buffer.Release({&FreeMapping, &DestroyMemory}); }
    static void AdoptWorkspace(ImportedImageBuffer& buffer, DeviceContext context, const ImageWorkspaceLayout& layout) {
        auto* storage = new std::byte[layout.required_allocation_bytes]{};
        Adopt(buffer);
        auto& resource = *buffer.resources_;
        resource.context.emplace(std::move(context));
        resource.mapped_base = reinterpret_cast<CUdeviceptr>(storage);
        resource.data = resource.mapped_base + layout.offset_bytes;
        resource.layout = layout;
        workspace_storage[resource.mapped_base] = 1U;
    }
    static std::shared_ptr<ImportedImageBuffer> AliasWorkspace(const ImportedImageBuffer& source, DeviceContext context) {
        auto result = std::make_shared<ImportedImageBuffer>();
        Adopt(*result);
        auto& resource = *result->resources_;
        resource.context.emplace(std::move(context));
        resource.mapped_base = source.resources_->mapped_base;
        resource.data = source.resources_->data;
        resource.layout = source.resources_->layout;
        ++workspace_storage.at(resource.mapped_base);
        return result;
    }
    static cudaError_t ReleaseWorkspace(ImportedImageBuffer& buffer) noexcept {
        auto* storage = buffer.resources_ ? reinterpret_cast<std::byte*>(buffer.resources_->mapped_base) : nullptr;
        const auto released = Release(buffer);
        if (released == cudaSuccess && storage) {
            const auto found = workspace_storage.find(reinterpret_cast<CUdeviceptr>(storage));
            if (found != workspace_storage.end() && --found->second == 0U) {
                workspace_storage.erase(found);
                delete[] storage;
            }
        }
        return released;
    }

   private:
    static CUresult FreeMapping(CUdeviceptr base) noexcept {
        ++unmaps;
        last_freed_base = base;
        return unmap_result;
    }
    static CUresult DestroyMemory(CUexternalMemory) noexcept {
        ++allocation_releases;
        return allocation_result;
    }
};

struct ImageWorkspaceTestAccess final {
    static inline std::exception_ptr initialize_failure;
    static inline std::exception_ptr alias_failure;
    static inline std::size_t initialized = 0U;

    static std::shared_ptr<ImageWorkspace> Create(DeviceContext display, ImageWorkspaceLayout layout) {
        return ImageWorkspace::Create(std::move(display), std::move(layout), {}, &operations);
    }
    static std::shared_ptr<ImageWorkspace> Create(std::shared_ptr<ImageCopyBackend> backend, ImageWorkspaceLayout layout) {
        return Create(DeviceContext(layout.device, std::move(backend)), std::move(layout));
    }
    static void Reset() noexcept {
        initialize_failure = {};
        alias_failure = {};
        initialized = 0U;
        ImportedImageBufferTestAccess::Reset();
    }
    [[nodiscard]] static ImageWorkspaceLayout Layout(const int device = 1) noexcept {
        return {.device_incarnation = 7U,
                .device_uuid = {1U},
                .device = device,
                .width = 4U,
                .height = 3U,
                .pitch_bytes = 64U,
                .required_allocation_bytes = 4096U,
                .alignment_bytes = 64U};
    }

   private:
    static std::shared_ptr<ImportedImageBuffer> Alias(const ImportedImageBuffer& buffer, DeviceContext context) {
        if (alias_failure) std::rethrow_exception(alias_failure);
        return ImportedImageBufferTestAccess::AliasWorkspace(buffer, std::move(context));
    }
    static void Initialize(ImportedImageBuffer& buffer, DeviceContext context, const ImageWorkspaceLayout& layout,
                           mmltk::common::io::ScopedFd, std::uint64_t) {
        ++initialized;
        ImportedImageBufferTestAccess::AdoptWorkspace(buffer, std::move(context), layout);
        if (initialize_failure) std::rethrow_exception(initialize_failure);
    }
    static inline const ImageWorkspace::Operations operations{
        &Initialize, &ImportedImageBufferTestAccess::ReleaseWorkspace, &Alias};
};

inline bool ContainsImageFailure(const std::exception_ptr& failure, const std::exception_ptr& expected) {
    if (failure == expected) return true;
    try {
        if (failure) std::rethrow_exception(failure);
    } catch (const ImageFailure& aggregate) {
        return ContainsImageFailure(aggregate.primary(), expected) || ContainsImageFailure(aggregate.secondary(), expected);
    } catch (...) {}
    return false;
}

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
        CreateEvent,
        AllocatePlane,
        Clear,
        Copy,
        RecordEvent,
        SynchronizeEvent,
        SynchronizeStream,
        NotifyStream,
        // CLEANUP-IGNORE: The fake backend's failure inventory is independent from its deliberately granular counters.
    };

    std::atomic<std::size_t> contexts_created{0U};
    // CLEANUP-IGNORE: Physical backend counters are independent from the controller worker lifecycle and publication gate.
    std::atomic<std::size_t> contexts_destroyed{0U};
    std::atomic<std::size_t> contexts_bound{0U};
    std::atomic<std::uintptr_t> last_bound_context{0U};
    std::atomic<std::size_t> streams_destroyed{0U};
    std::atomic<std::size_t> events_created{0U};
    std::atomic<std::size_t> events_destroyed{0U};
    std::size_t pitch_padding_bytes = 0U;
    std::atomic<std::size_t> planes_allocated{0U};
    std::atomic<std::size_t> planes_freed{0U};
    std::atomic<std::size_t> pinned_allocated{0U};
    std::atomic<int> pinned_receiver_device{-1};
    // CLEANUP-IGNORE: Synchronization and copy-path counters are distinct physical operations, not another allocation inventory.
    std::atomic<std::size_t> synchronized{0U};
    std::atomic<std::size_t> same_copies{0U};
    std::atomic<std::size_t> plane_clears{0U};
    std::atomic<std::uintptr_t> watched_copy_source{0U};
    std::atomic<std::size_t> watched_source_copies{0U};
    std::atomic<std::size_t> peer_copies{0U};
    std::atomic<std::size_t> staged_downloads{0U};
    std::atomic<std::size_t> staged_uploads{0U};
    bool peer_access = false;
    bool defer_events = false;
    std::atomic_bool defer_notifications{false};
    std::atomic_size_t workspace_finalizations{0U};
    [[nodiscard]] std::future<void> ObserveNextNotification() {
        auto observed = std::make_shared<std::promise<void>>();
        auto result = observed->get_future();
        std::scoped_lock lock(mutex_);
        notification_observer_ = std::move(observed);
        return result;
    }
    [[nodiscard]] std::future<std::uintptr_t> ObserveNextNotificationStream() {
        auto observed = std::make_shared<std::promise<std::uintptr_t>>();
        auto result = observed->get_future();
        std::scoped_lock lock(mutex_);
        notification_stream_observer_ = std::move(observed);
        return result;
    }
    [[nodiscard]] std::unique_ptr<mmltk::testsupport::TestGate> HoldStreamSettlements(std::string name, std::uintptr_t stream = 0U) {
        auto gate = std::make_unique<mmltk::testsupport::TestGate>(std::move(name));
        std::scoped_lock lock(mutex_);
        settlement_gates_.insert_or_assign(stream, gate->receipt());
        return gate;
    }
    void CompleteNotifications(std::uintptr_t stream = 0U, CUresult status = CUDA_SUCCESS) {
        std::vector<StreamNotification*> completed;
        {
            std::scoped_lock lock(mutex_);
            for (auto entry = notifications_.begin(); entry != notifications_.end();) {
                if (stream != 0U && entry->first != stream) {
                    ++entry;
                    continue;
                }
                completed.push_back(entry->second);
                entry = notifications_.erase(entry);
            }
        }
        for (auto* notification : completed) notification->Notify(status);
    }
    void SetStreamSettlement(std::uintptr_t stream, StreamSettlement result) {
        std::scoped_lock lock(mutex_);
        stream_settlements_.insert_or_assign(stream, std::move(result));
    }

    void FailAfter(const FailurePoint point, const std::size_t successful_calls = 0U, std::exception_ptr failure = {}) noexcept {
        failure_point_ = point;
        successful_calls_before_failure_ = successful_calls;
        persistent_failure_ = false;
        injected_failure_ = std::move(failure);
    }
    void FailPersistently(const FailurePoint point) noexcept {
        failure_point_ = point;
        successful_calls_before_failure_ = 0U;
        persistent_failure_ = true;
        injected_failure_ = {};
    }
    void FailDeviceBinding(const int device, std::exception_ptr failure) {
        std::scoped_lock lock(mutex_);
        failed_binding_device_ = device;
        device_binding_failure_ = std::move(failure);
    }

    void CompleteEvents() noexcept {
        {
            std::scoped_lock lock(mutex_);
            for (auto& event : events_)
                event.second = true;
        }
        event_ready_.notify_all();
    }
    [[nodiscard]] std::unique_ptr<mmltk::testsupport::TestGate> HoldSameDeviceCopies(std::string name) {
        auto gate = std::make_unique<mmltk::testsupport::TestGate>(std::move(name));
        std::scoped_lock lock(copy_mutex_);
        copy_gate_ = gate->receipt();
        return gate;
    }
    [[nodiscard]] std::unique_ptr<mmltk::testsupport::TestGate> HoldEventWaits(std::string name) {
        auto gate = std::make_unique<mmltk::testsupport::TestGate>(std::move(name));
        std::scoped_lock lock(mutex_);
        event_gate_ = gate->receipt();
        return gate;
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
        CheckDeviceBinding(context);
        last_bound_context.store(context, std::memory_order_release);
        ++contexts_bound;
    }
    [[nodiscard]] std::uintptr_t CreateStream(std::uintptr_t) override {
        MaybeFail(FailurePoint::CreateStream);
        return next_.fetch_add(1U);
    }
    void DestroyStream(std::uintptr_t, std::uintptr_t) noexcept override { ++streams_destroyed; }
    [[nodiscard]] std::uintptr_t CreateEvent(std::uintptr_t) override {
        MaybeFail(FailurePoint::CreateEvent);
        ++events_created;
        const auto event = next_.fetch_add(1U);
        std::scoped_lock lock(mutex_);
        events_[event] = !defer_events;
        return event;
    }
    void DestroyEvent(std::uintptr_t, const std::uintptr_t event) noexcept override {
        std::scoped_lock lock(mutex_);
        events_.erase(event);
        ++events_destroyed;
    }
    [[nodiscard]] ImagePlaneView AllocatePlane(std::uintptr_t, const ImagePlaneKind kind, const std::uint32_t width,
                                               const std::uint32_t height) override {
        MaybeFail(FailurePoint::AllocatePlane);
        const std::size_t pitch = static_cast<std::size_t>(width) * 4U + pitch_padding_bytes;
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
    void ClearPlane(std::uintptr_t, std::uintptr_t, const ImagePlaneView& plane) override {
        MaybeFail(FailurePoint::Clear);
        ++plane_clears;
        for (std::uint32_t row = 0U; row != plane.descriptor.height; ++row)
            std::memset(reinterpret_cast<std::byte*>(plane.data) + row * plane.descriptor.pitch_bytes, 0, plane.descriptor.row_bytes());
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
        std::optional<mmltk::testsupport::TestGate::Receipt> gate;
        {
            std::scoped_lock lock(copy_mutex_);
            gate = copy_gate_;
        }
        if (gate) gate->ArriveAndWait();
        CopyImagePlane(destination, source);
        if (source.data == watched_copy_source.load()) ++watched_source_copies;
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
    void NotifyStream(std::uintptr_t, std::uintptr_t stream, StreamNotification& notification) override {
        MaybeFail(FailurePoint::NotifyStream);
        std::shared_ptr<std::promise<void>> observed;
        std::shared_ptr<std::promise<std::uintptr_t>> stream_observed;
        bool deferred = false;
        {
            std::scoped_lock lock(mutex_);
            observed = std::exchange(notification_observer_, {});
            stream_observed = std::exchange(notification_stream_observer_, {});
            deferred = defer_notifications.load();
            if (deferred && !notifications_.emplace(stream, &notification).second)
                throw std::logic_error("fake stream notification is already pending");
        }
        if (!deferred) notification.Notify(CUDA_SUCCESS);
        if (observed) observed->set_value();
        if (stream_observed) stream_observed->set_value(stream);
    }
    StreamSettlement SettleStream(const std::uintptr_t context, std::uintptr_t stream) noexcept override {
        CompleteNotifications(stream);
        std::optional<mmltk::testsupport::TestGate::Receipt> gate;
        std::optional<StreamSettlement> result;
        {
            std::scoped_lock lock(mutex_);
            if (const auto found = settlement_gates_.find(stream); found != settlement_gates_.end())
                gate = found->second;
            else if (const auto all = settlement_gates_.find(0U); all != settlement_gates_.end())
                gate = all->second;
            if (const auto found = stream_settlements_.find(stream); found != stream_settlements_.end()) result = found->second;
        }
        if (gate) gate->ArriveAndWait();
        if (result) {
            ++synchronized;
            return *result;
        }
        try {
            MaybeFail(FailurePoint::Bind);
            CheckDeviceBinding(context);
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
    void CheckDeviceBinding(const std::uintptr_t context) {
        std::scoped_lock lock(mutex_);
        const auto found = devices_.find(context);
        if (device_binding_failure_ && found != devices_.end() && found->second == failed_binding_device_)
            std::rethrow_exception(device_binding_failure_);
    }
    void MaybeFail(const FailurePoint point) {
        if (failure_point_ != point) return;
        if (successful_calls_before_failure_ != 0U) {
            --successful_calls_before_failure_;
            return;
        }
        if (!persistent_failure_) failure_point_ = FailurePoint::None;
        if (injected_failure_) std::rethrow_exception(injected_failure_);
        throw std::runtime_error("injected image backend failure");
    }

    void Wait(const std::uintptr_t event) {
        std::unique_lock lock(mutex_);
        const auto gate = event_gate_;
        if (gate) {
            lock.unlock();
            gate->ArriveAndWait();
            lock.lock();
        }
        event_ready_.wait(lock, [this, event] {
            const auto found = events_.find(event);
            return found == events_.end() || found->second;
        });
    }

    std::atomic<std::uintptr_t> next_{1U};
    std::mutex mutex_;
    std::unordered_map<std::uintptr_t, StreamNotification*> notifications_;
    std::unordered_map<std::uintptr_t, StreamSettlement> stream_settlements_;
    std::shared_ptr<std::promise<void>> notification_observer_;
    std::shared_ptr<std::promise<std::uintptr_t>> notification_stream_observer_;
    std::unordered_map<std::uintptr_t, mmltk::testsupport::TestGate::Receipt> settlement_gates_;
    std::condition_variable event_ready_;
    std::mutex copy_mutex_;
    std::optional<mmltk::testsupport::TestGate::Receipt> copy_gate_;
    std::optional<mmltk::testsupport::TestGate::Receipt> event_gate_;
    std::unordered_map<std::uintptr_t, int> devices_;
    std::unordered_map<std::uintptr_t, bool> events_;
    FailurePoint failure_point_ = FailurePoint::None;
    std::size_t successful_calls_before_failure_ = 0U;
    bool persistent_failure_ = false;
    std::exception_ptr injected_failure_;
    int failed_binding_device_ = -1;
    std::exception_ptr device_binding_failure_;
};

[[nodiscard]] inline ImageWorkspaceFinalize FakeWorkspaceFinalizer(std::shared_ptr<FakeImageBackend> backend) {
    return [backend = std::move(backend)](ImagePlaneView clean, ImagePlaneView semantic, ImagePlaneView destination,
                                         ImageWorkspaceCoverage coverage, std::uintptr_t) {
        ++backend->workspace_finalizations;
        const auto draw = [&](ImageWorkspaceRegion region) {
            for (auto y = std::max(0, region.y1); y < std::min(static_cast<int>(clean.descriptor.height), region.y2); ++y) {
                for (auto x = std::max(0, region.x1); x < std::min(static_cast<int>(clean.descriptor.width), region.x2); ++x) {
                    const auto* base = reinterpret_cast<const std::uint8_t*>(clean.data) + y * clean.descriptor.pitch_bytes + x * 4U;
                    auto* pixel = reinterpret_cast<std::uint8_t*>(destination.data) + y * destination.descriptor.pitch_bytes + x * 4U;
                    std::memcpy(pixel, base, 4U);
                    if (!semantic.valid()) continue;
                    const auto* overlay = reinterpret_cast<const std::uint8_t*>(semantic.data) + y * semantic.descriptor.pitch_bytes + x * 4U;
                    if (overlay[3] == 0U) continue;
                    const float alpha = static_cast<float>(overlay[3]) / 255.0F;
                    for (unsigned channel = 0U; channel != 3U; ++channel)
                        pixel[channel] = static_cast<std::uint8_t>(base[channel] * (1.0F - alpha) + overlay[channel] * alpha);
                    pixel[3] = 255U;
                }
            }
        };
        if (coverage.full_image) draw({0, 0, static_cast<int>(clean.descriptor.width), static_cast<int>(clean.descriptor.height)});
        else for (auto region : coverage.regions) draw(region);
    };
}

[[nodiscard]] inline std::function<std::unique_ptr<SystemImageRuntime>(std::shared_ptr<ImageProductRevisionSequence>)> RuntimeFactory(
    const int device, const std::shared_ptr<FakeImageBackend>& backend, const ImageProductLayout layout = ImageProductLayout::Clean,
    std::function<std::unique_ptr<SystemImageModel>()> model = {}, const std::size_t output_buffer_count = 1U,
    ImageWorkspaceFinalize finalize = {}) {
    return
        [device, backend, layout, model = std::move(model), output_buffer_count,
         finalize = std::move(finalize)](std::shared_ptr<ImageProductRevisionSequence> revisions) {
            return std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{
                .device = device,
                .backend = backend,
                .model = model ? model() : nullptr,
                .input_layout = layout,
                .output_layout = layout,
                .output_buffer_count = output_buffer_count,
                .workspace_finalize = finalize,
                .product_revisions = std::move(revisions),
            });
        };
}

}  // namespace mmltk::frameworks::gpu::test_support
