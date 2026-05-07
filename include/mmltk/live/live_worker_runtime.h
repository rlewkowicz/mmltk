#pragma once

#include "live/live_helpers.h"
#include "mmltk_logging.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <thread>
#include <utility>
#include <string>
#include <vector>

namespace mmltk::live::worker_runtime {

template <typename RollbackFn>
class ScopedRollback {
   public:
    explicit ScopedRollback(RollbackFn rollback_fn) noexcept(std::is_nothrow_move_constructible_v<RollbackFn>)
        : rollback_fn_(std::move(rollback_fn)) {}

    ~ScopedRollback() {
        if (active_) {
            rollback_fn_();
        }
    }

    ScopedRollback(const ScopedRollback&) = delete;
    ScopedRollback& operator=(const ScopedRollback&) = delete;

    ScopedRollback(ScopedRollback&& other) noexcept(std::is_nothrow_move_constructible_v<RollbackFn>)
        : rollback_fn_(std::move(other.rollback_fn_)), active_(other.active_) {
        other.active_ = false;
    }

    ScopedRollback& operator=(ScopedRollback&& other) noexcept(std::is_nothrow_move_assignable_v<RollbackFn>) = delete;

    void dismiss() noexcept {
        active_ = false;
    }

    void run_and_dismiss() noexcept {
        if (!active_) {
            return;
        }
        rollback_fn_();
        active_ = false;
    }

   private:
    static_assert(std::is_nothrow_invocable_v<RollbackFn&>, "ScopedRollback requires noexcept rollback callables");

    RollbackFn rollback_fn_;
    bool active_ = true;
};

template <typename RollbackFn>
[[nodiscard]] inline auto make_scoped_rollback(RollbackFn&& rollback_fn) {
    return ScopedRollback<std::decay_t<RollbackFn>>(std::forward<RollbackFn>(rollback_fn));
}

template <typename ThreadMain>
inline void start_worker_thread(std::thread& thread, std::atomic<bool>& stop_requested, std::atomic<bool>& running,
                                ThreadMain&& thread_main, const char* already_running_context,
                                const char* restart_context) {
    if (running.load(std::memory_order_acquire)) {
        throw std::runtime_error(already_running_context);
    }
    if (thread.joinable()) {
        throw std::runtime_error(restart_context);
    }
    stop_requested.store(false, std::memory_order_release);
    running.store(true, std::memory_order_release);
    thread = std::thread(std::forward<ThreadMain>(thread_main));
}

inline void stop_worker_thread(std::thread& thread, std::atomic<bool>& stop_requested, std::atomic<bool>& running) {
    stop_requested.store(true, std::memory_order_release);
    if (thread.joinable()) {
        thread.join();
    }
    running.store(false, std::memory_order_release);
}

template <typename Status>
inline Status snapshot_status(const std::atomic<std::shared_ptr<const Status>>& status) {
    const std::shared_ptr<const Status> current = status.load(std::memory_order_acquire);
    return current ? *current : Status{};
}

template <typename Status>
inline void publish_status(std::atomic<std::shared_ptr<const Status>>* target, const Status& status) {
    if (target == nullptr) {
        return;
    }
    auto next = std::make_shared<Status>(status);
    std::shared_ptr<const Status> immutable = std::move(next);
    target->store(std::move(immutable), std::memory_order_release);
}

template <typename Slot, typename Output>
inline bool try_acquire_published_slot_view(Slot& slot, Output* out) {
    return mmltk::live::try_acquire_published_slot(slot, out, [](Slot& published) { return published.make_view(); });
}

template <typename Slot, typename Output>
inline bool try_acquire_latest_published_slot_view(std::vector<std::unique_ptr<Slot>>& slots,
                                                   const std::atomic<int>& latest_index, Output* out) {
    return mmltk::live::try_acquire_latest_published_slot(slots, latest_index, out,
                                                          [](Slot& published) { return published.make_view(); });
}

template <typename Slot, typename Output>
inline bool try_acquire_latest_ready_published_slot_view(std::vector<std::unique_ptr<Slot>>& slots,
                                                        const std::atomic<int>& latest_index, Output* out,
                                                        const char* event_context) {
    return mmltk::live::try_acquire_latest_ready_published_slot(
        slots, latest_index, out, [](Slot& published) { return published.make_view(); },
        [](Slot& published) { return published.ready_event_handle(); }, event_context);
}

template <typename Slot, typename Output>
inline bool try_acquire_latest_running_slot_view(std::vector<std::unique_ptr<Slot>>& slots,
                                                 const std::atomic<int>& latest_index, const std::atomic<bool>& running,
                                                 Output* out, const char* null_output_context) {
    if (out == nullptr) {
        throw std::runtime_error(null_output_context);
    }
    *out = {};
    if (!running.load(std::memory_order_acquire)) {
        return false;
    }
    return try_acquire_latest_published_slot_view(slots, latest_index, out);
}

template <typename Dimensions, typename Bundle, typename DeviceBuffer = BgrPitchedDeviceBuffer>
struct LiveFrameSlotCommon {
    using BundleType = Bundle;

    std::uint32_t slot_index = 0;
    DeviceBuffer device_buffer;
    CudaStreamHandle stream;
    CudaEventHandle ready_event;
    std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
    LiveFrameId frame_id{};
    LiveCaptureRegion region{};
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    bool short_frame = false;

    [[nodiscard]] BundleType make_view() const noexcept {
        BundleType bundle{
            slot_index,
            frame_id,
            device_buffer.data(),
            Dimensions{region.width, region.height, device_buffer.pitch_bytes()},
            ready_event.get(),
            stream.get(),
            region,
            capture_ns,
            ready_ns,
            short_frame,
        };
        if constexpr (requires(const DeviceBuffer& buffer, std::uint32_t width, std::uint32_t height) {
                          buffer.dmabuf_image(width, height);
                      }) {
            bundle.dmabuf_image = device_buffer.dmabuf_image(region.width, region.height);
        }
        return bundle;
    }

    void reset_for_reuse() noexcept {
        frame_id = {};
        region = {};
    }

    [[nodiscard]] cudaEvent_t ready_event_handle() const noexcept {
        return ready_event.get();
    }
};

template <typename Slot>
inline Slot* reserve_writable_slot_view(std::vector<std::unique_ptr<Slot>>& slots, std::atomic<int>& latest_index,
                                        const char* event_context) {
    return mmltk::live::reserve_writable_slot(
        slots, latest_index, [](Slot& slot) { slot.reset_for_reuse(); },
        [](Slot& slot) { return slot.ready_event_handle(); }, event_context);
}

inline void publish_error(std::atomic<std::shared_ptr<const std::string>>* target, std::string error_message) {
    store_error_message(target, std::move(error_message));
}

template <typename Slot, typename InitSlot>
inline void allocate_unique_slots(std::vector<std::unique_ptr<Slot>>& slots, std::uint32_t slot_count,
                                  InitSlot&& init_slot) {
    slots.reserve(slot_count);
    for (std::uint32_t slot_index = 0; slot_index < slot_count; ++slot_index) {
        auto slot = std::make_unique<Slot>();
        init_slot(*slot, slot_index);
        slots.push_back(std::move(slot));
    }
}

template <typename Slot>
inline void initialize_pitched_device_slot(Slot& slot, std::uint32_t slot_index, std::uint32_t width,
                                           std::uint32_t height, const char* buffer_context, const char* stream_context,
                                           const char* event_context) {
    slot.slot_index = slot_index;
    slot.device_buffer.ensure_dimensions(width, height, buffer_context);
    slot.stream.create_with_highest_priority(stream_context);
    slot.ready_event.create(cudaEventDisableTiming, event_context);
}

template <typename Slot>
inline void allocate_pitched_device_slots(std::vector<std::unique_ptr<Slot>>& slots, std::uint32_t slot_count,
                                          std::uint32_t width, std::uint32_t height, const char* buffer_context,
                                          const char* stream_context, const char* event_context) {
    allocate_unique_slots(slots, slot_count, [&](Slot& slot, std::uint32_t slot_index) {
        initialize_pitched_device_slot(slot, slot_index, width, height, buffer_context, stream_context, event_context);
    });
}

template <typename Slot>
inline void initialize_dmabuf_cuda_rgba_slot(Slot& slot, LinuxGpuInteropDevice& interop_device,
                                             const int cuda_device_index, const std::uint32_t slot_index,
                                             const std::uint32_t width, const std::uint32_t height,
                                             const char* buffer_context, const char* stream_context,
                                             const char* event_context) {
    slot.slot_index = slot_index;
    interop_device.ensure_open(cuda_device_index, buffer_context);
    slot.device_buffer.ensure_dimensions(interop_device, cuda_device_index, width, height, buffer_context);
    slot.stream.create_with_highest_priority(stream_context);
    slot.ready_event.create(cudaEventDisableTiming, event_context);
}

template <typename Slot>
inline void allocate_dmabuf_cuda_rgba_slots(std::vector<std::unique_ptr<Slot>>& slots, std::uint32_t slot_count,
                                            LinuxGpuInteropDevice& interop_device, const int cuda_device_index,
                                            const std::uint32_t width, const std::uint32_t height,
                                            const char* buffer_context, const char* stream_context,
                                            const char* event_context) {
    allocate_unique_slots(slots, slot_count, [&](Slot& slot, std::uint32_t slot_index) {
        initialize_dmabuf_cuda_rgba_slot(slot, interop_device, cuda_device_index, slot_index, width, height,
                                         buffer_context, stream_context, event_context);
    });
}

template <typename Slot, typename InitSlot>
inline void initialize_slot_array(std::vector<Slot>& slots, InitSlot&& init_slot) {
    for (std::size_t slot_index = 0; slot_index < slots.size(); ++slot_index) {
        init_slot(slots[slot_index], static_cast<std::uint32_t>(slot_index));
    }
}

template <typename Slot>
inline void release_slot_by_index(std::vector<std::unique_ptr<Slot>>& slots, const std::uint32_t slot_index,
                                  const char* out_of_range_context, const char* not_acquired_context) {
    if (slot_index >= slots.size()) {
        throw std::runtime_error(out_of_range_context);
    }
    release_acquired_slot(*slots[slot_index], not_acquired_context);
}

template <typename Slot>
inline void reset_slot_views_for_restart(std::vector<std::unique_ptr<Slot>>& slots, std::atomic<int>& latest_slot_index,
                                         const char* synchronize_context) {
    latest_slot_index.store(-1, std::memory_order_release);
    for (auto& slot : slots) {
        const std::uint32_t state = slot->state.load(std::memory_order_acquire);
        if (state == to_slot_state_value(SlotState::kAcquired)) {
            continue;
        }
        if (state == to_slot_state_value(SlotState::kPublished)) {
            const cudaEvent_t ready_event = slot->ready_event_handle();
            if (ready_event != nullptr) {
                ensure_cuda_ok(cudaEventSynchronize(ready_event), synchronize_context);
            }
        }
        slot->reset_for_reuse();
        slot->state.store(to_slot_state_value(SlotState::kFree), std::memory_order_release);
    }
}

template <typename Slot>
inline void publish_latest_slot(Slot& slot, std::atomic<int>& latest_slot_index) noexcept {
    slot.state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
    latest_slot_index.store(static_cast<int>(slot.slot_index), std::memory_order_release);
}

template <typename Slot>
inline void release_writing_slot(Slot& slot) noexcept {
    slot.state.store(to_slot_state_value(SlotState::kFree), std::memory_order_release);
}

template <typename Slot>
inline void initialize_pitched_device_resource(Slot& slot, std::uint32_t width, std::uint32_t height,
                                               const char* buffer_context, const char* stream_context,
                                               const char* event_context) {
    slot.device_buffer.ensure_dimensions(width, height, buffer_context);
    slot.stream.create_with_highest_priority(stream_context);
    slot.ready_event.create(cudaEventDisableTiming, event_context);
}

template <typename Slot>
inline void reset_pitched_device_resource(Slot& slot) noexcept {
    slot.ready_event.reset();
    slot.stream.reset();
    slot.device_buffer.reset();
}

template <typename Slot>
inline void initialize_pitched_event_resource(Slot& slot, std::uint32_t width, std::uint32_t height,
                                              const char* buffer_context, const char* event_context) {
    slot.device_buffer.ensure_dimensions(width, height, buffer_context);
    slot.ready_event.create(cudaEventDisableTiming, event_context);
}

template <typename Slot>
inline void reset_pitched_event_resource(Slot& slot) noexcept {
    slot.ready_event.reset();
    slot.device_buffer.reset();
}

template <typename Slot>
inline void initialize_stream_event_resource(Slot& slot, const char* stream_context, const char* event_context) {
    slot.stream.create_with_highest_priority(stream_context);
    slot.ready_event.create(cudaEventDisableTiming, event_context);
}

template <typename Slot>
inline void reset_stream_event_resource(Slot& slot) noexcept {
    slot.ready_event.reset();
    slot.stream.reset();
}

template <typename Status, typename LoopFn, typename ExHandlerFn>
inline void run_worker_thread_main(std::atomic<bool>& running_, std::atomic<bool>& stop_requested_,
                                   std::atomic<std::shared_ptr<const Status>>& status_, int cuda_device_index,
                                   const char* device_context, const char* logger_name, LoopFn&& loop_fn,
                                   ExHandlerFn&& ex_handler_fn) {
    Status status;
    status.running = true;

    try {
        if (device_context != nullptr) {
            ensure_cuda_ok(cudaSetDevice(cuda_device_index), device_context);
        }
        while (!stop_requested_.load(std::memory_order_acquire)) {
            std::forward<LoopFn>(loop_fn)(status);
        }
    } catch (const std::exception& error) {
        std::forward<ExHandlerFn>(ex_handler_fn)(error.what());
        status.last_error = error.what();
        if (logger_name != nullptr) {
            auto logger = mmltk::logging::logger(logger_name);
            logger->error("{}", error.what());
        }
    }

    status.running = false;
    publish_status(&status_, status);
    running_.store(false, std::memory_order_release);
}

}  // namespace mmltk::live::worker_runtime
