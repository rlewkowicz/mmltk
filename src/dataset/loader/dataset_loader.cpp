#include "dataset_loader_internal.h"
#include "execution_policy.h"
#include "profile_utils.h"
#include "mmltk_logging.h"
#include "worker_pool.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <numeric>
#include <random>
#include <stdexcept>

namespace mmltk {

namespace {

constexpr size_t kShuffleChunkImages = 8;
constexpr size_t kShuffleBlockImages = 256;

void validate_config(const DatasetLoader::Config& config) {
    if (config.compiled_path.empty()) {
        throw std::invalid_argument("compiled_path must not be empty");
    }
    if (config.batch_size == 0) {
        throw std::invalid_argument("batch_size must be greater than zero");
    }
    if (config.prefetch_factor <= 0) {
        throw std::invalid_argument("prefetch_factor must be greater than zero");
    }
    if (config.gather_workers < 0) {
        throw std::invalid_argument("gather_workers must be non-negative");
    }
    if (config.shuffle && config.gather_workers > 0 && config.gather_workers > config.prefetch_factor) {
        throw std::invalid_argument("gather_workers must not exceed prefetch_factor");
    }
    if (config.batch_shard_count == 0) {
        throw std::invalid_argument("batch_shard_count must be greater than zero");
    }
    if (config.batch_shard_rank >= config.batch_shard_count) {
        throw std::invalid_argument("batch_shard_rank must be less than batch_shard_count");
    }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void build_block_shuffled_order(std::vector<uint32_t>& order, uint32_t num_images, size_t batch_size,
                                std::mt19937_64& rng) {
    if (num_images == 0) {
        return;
    }

    const auto total_images = static_cast<size_t>(num_images);
    const size_t chunk_images = std::min(total_images, std::max(kShuffleChunkImages, batch_size));
    const size_t chunks_per_block = std::max<size_t>(1, kShuffleBlockImages / chunk_images);
    const size_t num_chunks = (total_images + chunk_images - 1) / chunk_images;
    const size_t num_blocks = (num_chunks + chunks_per_block - 1) / chunks_per_block;

    MMLTK_PROFILE_SET("loader.shuffle.chunk_images", chunk_images);
    MMLTK_PROFILE_SET("loader.shuffle.block_images", chunk_images * chunks_per_block);
    MMLTK_PROFILE_SET("loader.shuffle.chunks", num_chunks);
    MMLTK_PROFILE_SET("loader.shuffle.blocks", num_blocks);

    std::vector<size_t> block_order(num_blocks);
    std::iota(block_order.begin(), block_order.end(), 0);
    std::shuffle(block_order.begin(), block_order.end(), rng);

    std::vector<size_t> chunk_order(chunks_per_block);
    size_t out = 0;
    for (size_t block_id : block_order) {
        const size_t chunk_begin = block_id * chunks_per_block;
        const size_t chunk_end = std::min(chunk_begin + chunks_per_block, num_chunks);
        const size_t chunk_count = chunk_end - chunk_begin;
        chunk_order.resize(chunk_count);
        std::iota(chunk_order.begin(), chunk_order.end(), chunk_begin);
        std::shuffle(chunk_order.begin(), chunk_order.end(), rng);

        for (size_t chunk_id : chunk_order) {
            const size_t image_begin = chunk_id * chunk_images;
            const size_t image_end = std::min(image_begin + chunk_images, total_images);
            for (size_t image = image_begin; image < image_end; ++image) {
                order[out++] = static_cast<uint32_t>(image);
            }
        }
    }

    if (out != total_images) {
        throw std::runtime_error("block shuffle failed to populate the full epoch order");
    }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace

DatasetLoader::DatasetLoader(const Config& config) : impl_(std::make_unique<Impl>()) {
    MMLTK_PROFILE_SCOPE("loader.construct");
    validate_config(config);

    impl_->config = config;
    impl_->use_gather_path = true;
    MMLTK_PROFILE_SET("loader.prefetch_factor_requested", static_cast<size_t>(config.prefetch_factor));
    impl_->num_buffers = static_cast<size_t>(config.prefetch_factor);
    MMLTK_PROFILE_SET("loader.prefetch_factor", impl_->num_buffers);
    impl_->load_runtime_data();
    impl_->prescan();
    impl_->slots.resize(impl_->num_buffers);
    impl_->worker_cpus = resolve_cpu_affinity(config.cpu_affinity);
    if (impl_->use_gather_path) {
        const int requested_gather_workers = config.gather_workers > 0 ? config.gather_workers : config.prefetch_factor;
        const int clamped_gather_workers =
            clamp_worker_count_to_cpus(requested_gather_workers, impl_->worker_cpus.size(), 1, 1);
        log_worker_budget_clamp("loader.gather", requested_gather_workers, clamped_gather_workers, impl_->worker_cpus,
                                1, 1);
        impl_->gather_worker_count = static_cast<size_t>(clamped_gather_workers);
    } else {
        impl_->gather_worker_count = 0;
    }
    MMLTK_PROFILE_SET("loader.worker_cpu_count", impl_->worker_cpus.size());
    MMLTK_PROFILE_SET("loader.gather.worker_budget", impl_->gather_worker_count);
    MMLTK_PROFILE_SET("loader.worker_thread_count", impl_->gather_worker_count + static_cast<size_t>(1));

    const auto image_stride = static_cast<size_t>(impl_->header.image_stride);
    CudaStreamManagerConfig cuda_config{};
    cuda_config.batch_capacity = config.batch_size;
    cuda_config.image_stride = image_stride;
    cuda_config.num_buffers = impl_->num_buffers;
    cuda_config.enable_gather_buffers = impl_->use_gather_path;
    cuda_config.device_id = config.device_id;
    impl_->cuda_mgr = std::make_unique<CudaStreamManager>(cuda_config);

    impl_->order.resize(impl_->header.num_images);
    std::iota(impl_->order.begin(), impl_->order.end(), 0u);
    impl_->rebuild_batch_schedule();
    impl_->start_workers();
}

DatasetLoader::~DatasetLoader() {
    MMLTK_PROFILE_SCOPE("loader.destruct");
    {
        std::lock_guard<std::mutex> lock(impl_->slot_mtx);
        if (impl_->has_checked_out_slots_locked()) {
            mmltk::logging::logger("loader")->critical("DatasetLoader destroyed with unreleased batches");
            std::terminate();
        }
    }
    impl_->stop_workers();
    if (impl_->cuda_mgr) {
        impl_->cuda_mgr->sync();
    }
    impl_->cuda_mgr.reset();
}

void DatasetLoader::begin_epoch() {
    MMLTK_PROFILE_SCOPE("loader.begin_epoch");
    {
        std::lock_guard<std::mutex> lock(impl_->slot_mtx);
        if (impl_->has_checked_out_slots_locked()) {
            throw std::runtime_error("begin_epoch requires all checked-out batches to be released");
        }
    }
    while (true) {
        impl_->reclaim_reusable_slots();
        std::unique_lock<std::mutex> lock(impl_->slot_mtx);
        if (impl_->pipeline_idle_locked()) {
            break;
        }
        impl_->consume_cv.wait_for(lock, std::chrono::milliseconds(1));
    }

    if (impl_->config.shuffle) {
        MMLTK_PROFILE_SCOPE("loader.shuffle_order");
        std::mt19937_64 rng(impl_->config.seed++);
        build_block_shuffled_order(impl_->order, impl_->header.num_images, impl_->config.batch_size, rng);
    }
    impl_->rebuild_batch_schedule();

    impl_->reset_epoch_state();
    {
        std::lock_guard<std::mutex> lock(impl_->slot_mtx);
        impl_->refill_pipeline_locked();
    }
}

bool DatasetLoader::next_batch(Batch& out) {
    MMLTK_PROFILE_SCOPE("loader.next_batch");
    if (!impl_->epoch_initialized) {
        begin_epoch();
    }

    impl_->reclaim_reusable_slots();

    int ready_slot_idx = -1;
    size_t ready_batch_start = 0;
    size_t ready_num_images = 0;
    uint64_t ready_lease_id = 0;
    const float* ready_host_images = nullptr;
    {
        MMLTK_PROFILE_SCOPE("loader.wait_ready_batch");
        std::unique_lock<std::mutex> lock(impl_->slot_mtx);
        while (impl_->next_consume_batch < impl_->total_batches() &&
               !impl_->batch_ready_locked(impl_->next_consume_batch)) {
            lock.unlock();
            impl_->reclaim_reusable_slots();
            lock.lock();
            if (impl_->next_consume_batch >= impl_->total_batches() ||
                impl_->batch_ready_locked(impl_->next_consume_batch)) {
                break;
            }
            impl_->consume_cv.wait_for(lock, std::chrono::milliseconds(1));
        }

        if (impl_->next_consume_batch >= impl_->total_batches()) {
            return false;
        }

        const size_t slot_idx = impl_->batch_slot_by_id[impl_->next_consume_batch];
        ready_slot_idx = static_cast<int>(slot_idx);
        Impl::BatchSlot& slot = impl_->slots[slot_idx];
        ready_batch_start = slot.batch_start;
        ready_num_images = slot.num_images;
        ready_lease_id = slot.lease_id;
        ready_host_images = slot.host_images;
        slot.state = Impl::SlotState::CheckedOut;
        ++impl_->next_consume_batch;
    }

    out.num_images = ready_num_images;
    out.host_images = ready_host_images;
    out.device_images = impl_->cuda_mgr->device_buffer(ready_slot_idx);
    out.label_index = impl_->label_index.data();
    out.labels = impl_->labels.data();
    out.rle_pairs = impl_->rle_pairs.data();
    out.image_indices = impl_->order.data() + ready_batch_start;
    out.slot_index = static_cast<size_t>(ready_slot_idx);
    out.lease_id = ready_lease_id;
    return true;
}

void DatasetLoader::wait_batch(const Batch& batch) {
    {
        std::lock_guard<std::mutex> lock(impl_->slot_mtx);
        impl_->require_batch_slot_locked(batch, "wait_batch", {Impl::SlotState::CheckedOut, Impl::SlotState::Released},
                                         "a checked-out or released batch");
    }
    impl_->cuda_mgr->wait_for_transfer(static_cast<int>(batch.slot_index));
}

void DatasetLoader::handoff_batch(const Batch& batch, void* consumer_stream) {
    std::lock_guard<std::mutex> lock(impl_->slot_mtx);
    impl_->require_batch_slot_locked(batch, "handoff_batch", {Impl::SlotState::CheckedOut}, "a checked-out batch");
    impl_->cuda_mgr->handoff_to_stream(static_cast<int>(batch.slot_index), consumer_stream);
}

void DatasetLoader::release_batch(const Batch& batch) {
    {
        std::lock_guard<std::mutex> lock(impl_->slot_mtx);
        impl_->release_checked_out_batch_locked(batch, nullptr);
    }
    impl_->reclaim_reusable_slots();
}

void DatasetLoader::release_batch(const Batch& batch, void* consumer_stream) {
    {
        std::lock_guard<std::mutex> lock(impl_->slot_mtx);
        impl_->release_checked_out_batch_locked(batch, consumer_stream);
    }
    impl_->reclaim_reusable_slots();
}

size_t DatasetLoader::num_images() const {
    return impl_->header.num_images;
}
size_t DatasetLoader::num_batches() const {
    return impl_->total_batches();
}
uint32_t DatasetLoader::image_width() const {
    return impl_->header.image_width;
}
uint32_t DatasetLoader::image_height() const {
    return impl_->header.image_height;
}
uint32_t DatasetLoader::num_classes() const {
    return impl_->header.num_classes;
}

const char* DatasetLoader::class_name(uint32_t id) const {
    if (id >= impl_->header.num_classes) {
        throw std::out_of_range("class id out of range");
    }
    return impl_->header.class_names[id].data();
}

size_t DatasetLoader::image_stride() const {
    return impl_->header.image_stride;
}
size_t DatasetLoader::num_label_instances() const {
    return impl_->labels.size();
}
size_t DatasetLoader::num_rle_pairs() const {
    return impl_->rle_pairs.size();
}

const float* DatasetLoader::pixel_blob() const {
    return impl_->pixels;
}
const LabelIndexEntry* DatasetLoader::label_index() const {
    return impl_->label_index.data();
}
const PackedInstance* DatasetLoader::label_data() const {
    return impl_->labels.data();
}
const RLEPair* DatasetLoader::rle_data() const {
    return impl_->rle_pairs.data();
}

CudaStreamManager& DatasetLoader::cuda() const {
    return *impl_->cuda_mgr;
}

void DatasetLoader::Impl::rebuild_batch_schedule() {
    batch_starts.clear();
    const auto total_images = static_cast<size_t>(header.num_images);
    if (total_images == 0) {
        return;
    }

    const size_t global_batches = config.drop_last ? (total_images / config.batch_size)
                                                   : ((total_images + config.batch_size - 1) / config.batch_size);
    const auto shard_count = static_cast<size_t>(config.batch_shard_count);
    const auto shard_rank = static_cast<size_t>(config.batch_shard_rank);
    batch_starts.reserve((global_batches + shard_count - 1) / shard_count);
    for (size_t global_batch = shard_rank; global_batch < global_batches; global_batch += shard_count) {
        batch_starts.push_back(global_batch * config.batch_size);
    }

    MMLTK_PROFILE_SET("loader.batch_shard_rank", shard_rank);
    MMLTK_PROFILE_SET("loader.batch_shard_count", shard_count);
    MMLTK_PROFILE_SET("loader.batch_shard_batches", batch_starts.size());
}

bool DatasetLoader::Impl::slot_state_matches(SlotState state, std::initializer_list<SlotState> allowed_states) const {
    for (const auto allowed_state : allowed_states) {
        if (state == allowed_state) {
            return true;
        }
    }
    return false;
}

DatasetLoader::Impl::BatchSlot& DatasetLoader::Impl::require_batch_slot_locked(
    const Batch& batch, const char* operation, std::initializer_list<SlotState> allowed_states,
    const char* required_state_message) {
    BatchSlot& slot = checked_out_slot_locked(batch, operation);
    if (!slot_state_matches(slot.state, allowed_states)) {
        throw std::runtime_error(std::string(operation) + " requires " + required_state_message);
    }
    return slot;
}

void DatasetLoader::Impl::release_checked_out_batch_locked(const Batch& batch, void* consumer_stream) {
    BatchSlot& slot = require_batch_slot_locked(batch, "release_batch", {SlotState::CheckedOut}, "a checked-out batch");
    if (consumer_stream) {
        cuda_mgr->record_consumer_done(static_cast<int>(batch.slot_index), consumer_stream);
    }
    slot.state = SlotState::Released;
}

}  // namespace mmltk
