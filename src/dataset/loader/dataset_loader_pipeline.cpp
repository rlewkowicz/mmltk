#include "dataset_loader_internal.h"
#include "execution_policy.h"
#include "profile_utils.h"
#include "worker_pool.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace fastloader {

namespace {

template <typename SlotCollection>
auto& checked_out_slot_impl(SlotCollection& slots,
                            const Batch& batch,
                            const char* operation) {
    if (batch.slot_index >= slots.size()) {
        throw std::runtime_error(std::string(operation) + ": batch slot index out of range");
    }
    auto& slot = slots[batch.slot_index];
    if (slot.lease_id != batch.lease_id) {
        throw std::runtime_error(std::string(operation) + ": stale batch lease");
    }
    return slot;
}

bool batch_order_is_contiguous(const uint32_t* batch_order, size_t num_images) {
    if (num_images == 0) {
        return true;
    }
    const uint32_t first = batch_order[0];
    for (size_t i = 1; i < num_images; ++i) {
        if (batch_order[i] != first + i) {
            return false;
        }
    }
    return true;
}

void copy_batch_images(float* dst,
                       const float* pixels,
                       const uint32_t* batch_order,
                       size_t num_images,
                       size_t image_stride) {
    const size_t stride_floats = image_stride / sizeof(float);
    size_t image = 0;
    size_t runs = 0;
    size_t contiguous_images = 0;

    while (image < num_images) {
        const size_t run_begin = image;
        const uint32_t source_begin = batch_order[run_begin];
        size_t run_length = 1;
        while (run_begin + run_length < num_images &&
               batch_order[run_begin + run_length] == source_begin + run_length) {
            ++run_length;
        }

        std::memcpy(dst + run_begin * stride_floats,
                    pixels + static_cast<size_t>(source_begin) * stride_floats,
                    run_length * image_stride);
        ++runs;
        contiguous_images += run_length;
        image += run_length;
    }

    FASTLOADER_PROFILE_ADD("loader.gather.runs", runs);
    FASTLOADER_PROFILE_ADD("loader.gather.contiguous_images", contiguous_images);
}

} // namespace

void DatasetLoader::Impl::reset_slot(BatchSlot& slot) {
    slot.batch_id = 0;
    slot.batch_start = 0;
    slot.num_images = 0;
    slot.lease_id = 0;
    slot.host_images = nullptr;
    slot.transfer_images = nullptr;
    slot.state = SlotState::Free;
}

void DatasetLoader::Impl::prepare_slot(BatchSlot& slot, size_t batch_id) {
    slot.batch_id = batch_id;
    slot.batch_start = batch_start_for(batch_id);
    slot.num_images =
        std::min(config.batch_size, static_cast<size_t>(header.num_images) - slot.batch_start);
    slot.lease_id = next_lease_id++;
    if (use_gather_path) {
        const bool contiguous = batch_order_is_contiguous(order.data() + slot.batch_start, slot.num_images);
        if (contiguous) {
            const size_t stride_floats = header.image_stride / sizeof(float);
            const size_t pixel_start = static_cast<size_t>(order[slot.batch_start]);
            slot.host_images = pixels + pixel_start * stride_floats;
        } else {
            slot.host_images = nullptr;
        }
        slot.transfer_images = nullptr;
        slot.state = SlotState::Queued;
    } else {
        const size_t stride_floats = header.image_stride / sizeof(float);
        const size_t pixel_start = static_cast<size_t>(order[slot.batch_start]);
        slot.host_images = pixels + pixel_start * stride_floats;
        slot.transfer_images = slot.host_images;
        slot.state = SlotState::HostReady;
    }
}

DatasetLoader::Impl::BatchSlot& DatasetLoader::Impl::checked_out_slot_locked(
    const Batch& batch,
    const char* operation) {
    return checked_out_slot_impl(slots, batch, operation);
}

const DatasetLoader::Impl::BatchSlot& DatasetLoader::Impl::checked_out_slot_locked(
    const Batch& batch,
    const char* operation) const {
    return checked_out_slot_impl(slots, batch, operation);
}

bool DatasetLoader::Impl::batch_ready_locked(size_t batch_id) const {
    if (batch_id >= batch_slot_by_id.size()) {
        return false;
    }
    const size_t slot_idx = batch_slot_by_id[batch_id];
    if (slot_idx == kInvalidSlot || slot_idx >= slots.size()) {
        return false;
    }
    const BatchSlot& slot = slots[slot_idx];
    return slot.batch_id == batch_id && slot.state == SlotState::TransferQueued;
}

bool DatasetLoader::Impl::has_checked_out_slots_locked() const {
    for (const BatchSlot& slot : slots) {
        if (slot.state == SlotState::CheckedOut) {
            return true;
        }
    }
    return false;
}

int DatasetLoader::Impl::acquire_transferable_slot_locked() const {
    if (transfer_queue.empty()) {
        return -1;
    }
    return static_cast<int>(transfer_queue.front());
}

void DatasetLoader::Impl::notify_slot_prepared_locked(size_t slot_idx) {
    BatchSlot& slot = slots[slot_idx];
    if (slot.state == SlotState::Queued) {
        gather_queue.push_back(slot_idx);
        gather_cv.notify_one();
        return;
    }
    if (slot.state == SlotState::HostReady) {
        transfer_queue.push_back(slot_idx);
        transfer_cv.notify_one();
    }
}

void DatasetLoader::Impl::prefetch_worker() {
    while (true) {
        size_t slot_idx = 0;
        BatchSlot slot_snapshot;
        {
            std::unique_lock<std::mutex> lock(slot_mtx);
            gather_cv.wait(lock, [this] {
                return shutdown.load() || !gather_queue.empty();
            });
            if (shutdown.load()) {
                return;
            }
            slot_idx = gather_queue.front();
            gather_queue.pop_front();
            BatchSlot& slot = slots[slot_idx];
            if (slot.state != SlotState::Queued) {
                continue;
            }
            slot.state = SlotState::Filling;
            slot_snapshot = slot;
        }

        const size_t stride = header.image_stride;
        FASTLOADER_PROFILE_SCOPE("loader.gather_batch");
        float* dst = cuda_mgr->gather_buffer(static_cast<int>(slot_idx));
        FASTLOADER_PROFILE_ADD("loader.gather.images", slot_snapshot.num_images);
        FASTLOADER_PROFILE_ADD("loader.gather.bytes", slot_snapshot.num_images * stride);
        copy_batch_images(dst,
                          pixels,
                          order.data() + slot_snapshot.batch_start,
                          slot_snapshot.num_images,
                          stride);

        {
            std::lock_guard<std::mutex> lock(slot_mtx);
            BatchSlot& ready_slot = slots[slot_idx];
            if (ready_slot.batch_id != slot_snapshot.batch_id ||
                ready_slot.state != SlotState::Filling) {
                continue;
            }
            if (ready_slot.host_images == nullptr) {
                ready_slot.host_images = dst;
            }
            ready_slot.transfer_images = dst;
            ready_slot.state = SlotState::HostReady;
            transfer_queue.push_back(slot_idx);
        }
        transfer_cv.notify_one();
    }
}

void DatasetLoader::Impl::transfer_worker() {
    while (true) {
        int slot_idx = -1;
        BatchSlot slot_snapshot;
        {
            std::unique_lock<std::mutex> lock(slot_mtx);
            transfer_cv.wait(lock, [this] {
                return shutdown.load() || acquire_transferable_slot_locked() >= 0;
            });
            if (shutdown.load()) {
                return;
            }
            slot_idx = acquire_transferable_slot_locked();
            if (slot_idx < 0) {
                continue;
            }
            transfer_queue.pop_front();
            BatchSlot& slot = slots[static_cast<size_t>(slot_idx)];
            if (slot.state != SlotState::HostReady) {
                continue;
            }
            slot.state = SlotState::TransferSubmitting;
            slot_snapshot = slot;
        }

        const size_t bytes = slot_snapshot.num_images * header.image_stride;
        FASTLOADER_PROFILE_ADD("loader.h2d.bytes", bytes);
        cuda_mgr->async_h2d(slot_idx, slot_snapshot.transfer_images, bytes);

        {
            std::lock_guard<std::mutex> lock(slot_mtx);
            BatchSlot& slot = slots[static_cast<size_t>(slot_idx)];
            if (slot.batch_id != slot_snapshot.batch_id ||
                slot.state != SlotState::TransferSubmitting) {
                continue;
            }
            slot.state = SlotState::TransferQueued;
        }
        consume_cv.notify_one();
    }
}

void DatasetLoader::Impl::start_workers() {
    FASTLOADER_PROFILE_SCOPE("loader.start_workers");
    if (!workers.empty() || transfer_worker_thread.joinable()) {
        return;
    }
    if (use_gather_path) {
        if (gather_worker_count == 0) {
            throw std::runtime_error("shuffle path requires at least one gather worker");
        }
        FASTLOADER_PROFILE_SET("loader.gather.worker_count", gather_worker_count);
        workers.reserve(gather_worker_count);
        for (size_t i = 0; i < gather_worker_count; ++i) {
            workers.emplace_back([this, i] {
                apply_worker_execution_policy(ExecutionPolicyRequest{
                    worker_cpus,
                    "fl_gthr" + std::to_string(i),
                    i,
                    true,
                    true,
                });
                prefetch_worker();
            });
        }
    } else {
        FASTLOADER_PROFILE_SET("loader.gather.worker_count", 0);
    }
    transfer_worker_thread = std::thread([this] {
        apply_worker_execution_policy(ExecutionPolicyRequest{
            worker_cpus,
            "fl_h2d",
            gather_worker_count,
            true,
            true,
        });
        transfer_worker();
    });
}

void DatasetLoader::Impl::stop_workers() {
    FASTLOADER_PROFILE_SCOPE("loader.stop_workers");
    shutdown.store(true);
    gather_cv.notify_all();
    transfer_cv.notify_all();
    consume_cv.notify_all();
    for (auto& worker : workers) {
        worker.join();
    }
    workers.clear();
    if (transfer_worker_thread.joinable()) {
        transfer_worker_thread.join();
    }
    shutdown.store(false);
}

void DatasetLoader::Impl::reset_epoch_state() {
    FASTLOADER_PROFILE_SCOPE("loader.reset_epoch_state");
    if (cuda_mgr) {
        cuda_mgr->sync();
    }

    next_submit_batch = 0;
    next_consume_batch = 0;
    epoch_initialized = true;

    std::lock_guard<std::mutex> lock(slot_mtx);
    batch_slot_by_id.assign(total_batches(), kInvalidSlot);
    free_slots.clear();
    gather_queue.clear();
    transfer_queue.clear();
    for (BatchSlot& slot : slots) {
        reset_slot(slot);
    }
    for (size_t slot_idx = 0; slot_idx < slots.size(); ++slot_idx) {
        free_slots.push_back(slot_idx);
    }
}

void DatasetLoader::Impl::reclaim_reusable_slots() {
    while (true) {
        std::vector<size_t> released_slots;
        {
            std::lock_guard<std::mutex> lock(slot_mtx);
            for (size_t slot_idx = 0; slot_idx < slots.size(); ++slot_idx) {
                if (slots[slot_idx].state == SlotState::Released) {
                    released_slots.push_back(slot_idx);
                }
            }
        }
        if (released_slots.empty()) {
            return;
        }

        bool reclaimed_any = false;
        for (size_t slot_idx : released_slots) {
            if (!cuda_mgr->slot_reusable(static_cast<int>(slot_idx))) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(slot_mtx);
                BatchSlot& slot = slots[slot_idx];
                if (slot.state != SlotState::Released) {
                    continue;
                }
                if (slot.batch_id < batch_slot_by_id.size() &&
                    batch_slot_by_id[slot.batch_id] == slot_idx) {
                    batch_slot_by_id[slot.batch_id] = kInvalidSlot;
                }
                reset_slot(slot);
                free_slots.push_back(slot_idx);
                reclaimed_any = true;
            }
        }
        if (!reclaimed_any) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(slot_mtx);
            refill_pipeline_locked();
        }
    }
}

bool DatasetLoader::Impl::pipeline_idle_locked() const {
    if (!gather_queue.empty() || !transfer_queue.empty()) {
        return false;
    }
    for (const BatchSlot& slot : slots) {
        if (slot.state != SlotState::Free) {
            return false;
        }
    }
    return true;
}

void DatasetLoader::Impl::refill_pipeline_locked() {
    FASTLOADER_PROFILE_SCOPE("loader.refill_pipeline");
    const size_t total = total_batches();
    size_t refilled_slots = 0;
    while (next_submit_batch < total && !free_slots.empty()) {
        const size_t slot_idx = free_slots.front();
        free_slots.pop_front();
        BatchSlot& slot = slots[slot_idx];
        prepare_slot(slot, next_submit_batch);
        batch_slot_by_id[next_submit_batch] = slot_idx;
        notify_slot_prepared_locked(slot_idx);
        ++next_submit_batch;
        ++refilled_slots;
    }
    FASTLOADER_PROFILE_ADD("loader.refill.slots", refilled_slots);
}

} // namespace fastloader
