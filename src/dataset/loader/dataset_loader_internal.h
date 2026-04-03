#pragma once

#include "common_utils.h"
#include "debug_utils.h"
#include "dataset_loader.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace mmltk {

struct DatasetLoader::Impl {
    Config config;

    FileHeader header{};
    OwnedBuffer<LabelIndexEntry> label_index;
    OwnedBuffer<PackedInstance> labels;
    OwnedBuffer<RLEPair> rle_pairs;
    MappedFile pixel_file;
    const float* pixels = nullptr;

    std::unique_ptr<CudaStreamManager> cuda_mgr;
    size_t num_buffers = 0;
    bool epoch_initialized = false;

    std::vector<uint32_t> order;
    std::vector<size_t> batch_starts;
    size_t next_submit_batch = 0;
    size_t next_consume_batch = 0;
    uint64_t next_lease_id = 1;
    std::vector<size_t> batch_slot_by_id;
    std::deque<size_t> free_slots;
    std::deque<size_t> gather_queue;
    std::deque<size_t> transfer_queue;

    std::atomic<bool> shutdown{false};
    std::vector<std::thread> workers;
    std::thread transfer_worker_thread;
    std::vector<int> worker_cpus;
    size_t gather_worker_count = 0;
    bool use_gather_path = true;
    static constexpr size_t kInvalidSlot = std::numeric_limits<size_t>::max();

    enum class SlotState : uint8_t {
        Free,
        Queued,
        Filling,
        HostReady,
        TransferSubmitting,
        TransferQueued,
        CheckedOut,
        Released,
    };

    struct BatchSlot {
        size_t batch_id = 0;
        size_t batch_start = 0;
        size_t num_images = 0;
        uint64_t lease_id = 0;
        const float* host_images = nullptr;
        const float* transfer_images = nullptr;
        SlotState state = SlotState::Free;
    };

    std::mutex slot_mtx;
    std::condition_variable gather_cv;
    std::condition_variable transfer_cv;
    std::condition_variable consume_cv;
    std::vector<BatchSlot> slots;

    [[nodiscard]] size_t total_batches() const {
        return batch_starts.size();
    }

    [[nodiscard]] size_t batch_start_for(size_t batch_id) const {
        return batch_starts[batch_id];
    }

    void load_runtime_data();
    void prescan();
    void rebuild_batch_schedule();

    void reset_slot(BatchSlot& slot);
    void prepare_slot(BatchSlot& slot, size_t batch_id);
    [[nodiscard]] int acquire_transferable_slot_locked() const;
    void notify_slot_prepared_locked(size_t slot_idx);
    [[nodiscard]] bool slot_state_matches(SlotState state,
                                          std::initializer_list<SlotState> allowed_states) const;
    BatchSlot& require_batch_slot_locked(const Batch& batch,
                                         const char* operation,
                                         std::initializer_list<SlotState> allowed_states,
                                         const char* required_state_message);
    BatchSlot& checked_out_slot_locked(const Batch& batch, const char* operation);
    const BatchSlot& checked_out_slot_locked(const Batch& batch, const char* operation) const;
    void release_checked_out_batch_locked(const Batch& batch, void* consumer_stream);
    [[nodiscard]] bool batch_ready_locked(size_t batch_id) const;
    [[nodiscard]] bool has_checked_out_slots_locked() const;
    void prefetch_worker();
    void transfer_worker();
    void start_workers();
    void stop_workers();
    void reset_epoch_state();
    void reclaim_reusable_slots();
    [[nodiscard]] bool pipeline_idle_locked() const;
    void refill_pipeline_locked();
};

} // namespace mmltk
