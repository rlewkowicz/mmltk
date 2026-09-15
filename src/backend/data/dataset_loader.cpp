#include "src/backend/data/dataset_loader.h"

#include <cuda_runtime_api.h>
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "src/backend/data/compiled_image_stream.h"
#include "src/common/system/cpu_affinity.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_error.h"

import mmltk.common.logging.profile_utils;

namespace mmltk::backend::data {

using mmltk::common::system::clamp_worker_count_to_cpus;

namespace {

constexpr size_t kShuffleChunkImages = 8;
constexpr size_t kShuffleBlockImages = 256;

void validate_config(const DatasetLoader::Config& config) {
    if (config.compiled_path.empty()) { throw std::invalid_argument("compiled_path must not be empty"); }
    if (config.batch_size == 0) { throw std::invalid_argument("batch_size must be greater than zero"); }
    if (config.prefetch_factor <= 0) { throw std::invalid_argument("prefetch_factor must be greater than zero"); }
    if (config.gather_workers < 0) { throw std::invalid_argument("gather_workers must be non-negative"); }
    if (config.shuffle && config.gather_workers > 0 && config.gather_workers > config.prefetch_factor) {
        throw std::invalid_argument("gather_workers must not exceed prefetch_factor");
    }
    if (config.batch_shard_count == 0) { throw std::invalid_argument("batch_shard_count must be greater than zero"); }
    if (config.batch_shard_rank >= config.batch_shard_count) {
        throw std::invalid_argument("batch_shard_rank must be less than batch_shard_count");
    }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void build_block_shuffled_order(std::vector<uint32_t>& order, uint32_t num_images, size_t batch_size, std::mt19937_64& rng,
                                std::vector<size_t>& block_order, std::vector<size_t>& chunk_order) {
    if (num_images == 0) { return; }

    const auto total_images = static_cast<size_t>(num_images);
    const size_t chunk_images = std::min(total_images, std::max(kShuffleChunkImages, batch_size));
    const size_t chunks_per_block = std::max<size_t>(1, kShuffleBlockImages / chunk_images);
    const size_t num_chunks = (total_images + chunk_images - 1) / chunk_images;
    const size_t num_blocks = (num_chunks + chunks_per_block - 1) / chunks_per_block;

    mmltk::common::logging::profile_set_value("loader.shuffle.chunk_images", chunk_images);
    mmltk::common::logging::profile_set_value("loader.shuffle.block_images", chunk_images * chunks_per_block);
    mmltk::common::logging::profile_set_value("loader.shuffle.chunks", num_chunks);
    mmltk::common::logging::profile_set_value("loader.shuffle.blocks", num_blocks);

    block_order.resize(num_blocks);
    std::iota(block_order.begin(), block_order.end(), 0);
    std::shuffle(block_order.begin(), block_order.end(), rng);

    chunk_order.resize(chunks_per_block);
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

    if (out != total_images) { throw std::runtime_error("block shuffle failed to populate the full epoch order"); }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace

struct DatasetLoader::Impl {
    enum class State : uint8_t { Free, Reading, Ready, CheckedOut, Released };
    struct Slot {
        State state = State::Free;
        size_t batch = 0, start = 0, count = 0;
        uint64_t lease = 0;
        bool transfer_pending = false, consumer_pending = false;
        const float* host = nullptr;
        std::vector<CompiledImageRead> reads;
    };
    Config config;
    CompiledDataset source;
    std::unique_ptr<CompiledImageStream> stream;
    std::vector<uint32_t> order;
    std::vector<size_t> shuffled_blocks, shuffled_chunks;
    std::vector<size_t> batch_starts;
    std::vector<Slot> slots;
    std::vector<size_t> batch_slots;
    size_t submitted = 0, consumed = 0;
    uint64_t next_lease = 1;
    bool epoch = false, stopping = false, resetting = false;
    std::exception_ptr failure;
    std::mutex mutex;
    std::condition_variable changed;

    void check_failure() const {
        if (failure) std::rethrow_exception(failure);
        if (stopping) throw std::runtime_error("dataset loader stopped");
    }
    bool checked_out() const {
        return std::ranges::any_of(slots, [](const Slot& slot) { return slot.state == State::CheckedOut; });
    }
    Slot& require(const Batch& batch, const char* operation, bool released = false) {
        if (batch.owner != this) throw std::runtime_error(std::string(operation) + ": foreign batch lease");
        if (batch.slot_index >= slots.size()) throw std::runtime_error(std::string(operation) + ": batch slot index out of range");
        auto& slot = slots[batch.slot_index];
        if (slot.lease != batch.lease_id) throw std::runtime_error(std::string(operation) + ": stale batch lease");
        if (slot.state != State::CheckedOut && !(released && slot.state == State::Released))
            throw std::runtime_error(std::string(operation) + ": batch is not checked out");
        return slot;
    }
    static void read_complete(void* context, const size_t index, std::exception_ptr error, const bool read) noexcept {
        auto& owner = *static_cast<Impl*>(context);
        {
            std::lock_guard lock(owner.mutex);
            if (error && !owner.failure) owner.failure = error;
            auto& slot = owner.slots[index];
            if (error || !read) {
                slot.state = State::Released;
                slot.transfer_pending = false;
            } else {
                slot.state = State::Ready;
            }
        }
        owner.changed.notify_all();
    }
    void completed(const size_t index, const std::exception_ptr error, const bool consumer) noexcept {
        {
            std::lock_guard lock(mutex);
            if (error && !failure) failure = error;
            (consumer ? slots[index].consumer_pending : slots[index].transfer_pending) = false;
            if (!failure && !stopping && !resetting) {
                try {
                    refill(index);
                } catch (...) { failure = std::current_exception(); }
            }
        }
        changed.notify_all();
    }
    static void transfer_complete(void* context, const size_t index, std::exception_ptr error) noexcept {
        static_cast<Impl*>(context)->completed(index, error, false);
    }
    static void consumer_complete(void* context, const size_t index, std::exception_ptr error) noexcept {
        static_cast<Impl*>(context)->completed(index, error, true);
    }
    void rebuild_schedule() {
        batch_starts.clear();
        const size_t images = source.header().num_images;
        const size_t batches =
            config.drop_last ? images / config.batch_size : images / config.batch_size + (images % config.batch_size != 0);
        const size_t local_batches =
            config.batch_shard_rank < batches ? (batches - 1 - config.batch_shard_rank) / config.batch_shard_count + 1 : 0;
        batch_starts.reserve(local_batches);
        for (size_t batch = config.batch_shard_rank; batch < batches; batch += config.batch_shard_count)
            batch_starts.push_back(batch * config.batch_size);
        batch_slots.resize(batch_starts.size());
    }
    void refill(const size_t index) {
        check_failure();
        auto& slot = slots[index];
        if (slot.state == State::Released && !slot.transfer_pending && !slot.consumer_pending) slot.state = State::Free;
        if (submitted == batch_starts.size() || slot.state != State::Free) return;
        slot.batch = submitted;
        slot.start = batch_starts[submitted];
        slot.count = std::min(config.batch_size, order.size() - slot.start);
        slot.lease = next_lease++;
        slot.reads.clear();
        bool contiguous = true;
        for (size_t image = 0; image < slot.count; ++image) {
            slot.reads.push_back({order[slot.start + image], image * source.header().image_stride});
            contiguous &= order[slot.start + image] == order[slot.start] + image;
        }
        slot.host = contiguous ? source.image_pixels(order[slot.start]) : nullptr;
        slot.transfer_pending = true;
        slot.state = State::Reading;
        batch_slots[submitted++] = index;
        try {
            stream->submit(index, source, slot.reads, {.context = this, .complete = read_complete},
                           {.context = this, .complete = transfer_complete});
        } catch (...) {
            slot.transfer_pending = false;
            slot.state = State::Released;
            failure = std::current_exception();
            changed.notify_all();
            throw;
        }
    }
    void release(const Batch& batch, void* consumer_stream, const bool has_consumer) {
        std::lock_guard lock(mutex);
        auto& slot = require(batch, "release_batch");
        if (has_consumer && stopping) {
            mmltk::frameworks::gpu::CudaDeviceScope scope(config.device_id);
            mmltk::frameworks::gpu::ensure_cuda_ok(scope.status(), "stopped dataset device binding");
            const auto settled = cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(consumer_stream));
            mmltk::frameworks::gpu::ensure_cuda_ok(scope.Finalize(), "stopped dataset caller restoration");
            mmltk::frameworks::gpu::ensure_cuda_ok(settled, "stopped dataset consumer completion");
        } else if (has_consumer) {
            stream->release(batch.slot_index, consumer_stream, {.context = this, .complete = consumer_complete});
            slot.consumer_pending = true;
        }
        slot.state = State::Released;
        if (!failure && !stopping && !resetting) {
            try {
                refill(batch.slot_index);
            } catch (...) { failure = std::current_exception(); }
        }
        changed.notify_all();
    }
};

DatasetLoader::DatasetLoader(const Config& config, std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement,
                             decltype(&cudaEventRecord) record_consumer) : impl_(std::make_unique<Impl>()) {
    validate_config(config);
    if (!record_consumer) throw std::invalid_argument("dataset consumer completion operation is unavailable");
    auto& state = *impl_;
    state.config = config;
    state.source = CompiledDataset::open(
        config.compiled_path, config.shuffle ? CompiledDataset::AccessPattern::Normal : CompiledDataset::AccessPattern::Sequential);
    state.order.resize(state.source.header().num_images);
    std::iota(state.order.begin(), state.order.end(), 0U);
    state.rebuild_schedule();
    const auto execution = config.execution ? *config.execution
                                            : mmltk::frameworks::gpu::resolve_device_execution(
                                                  config.device_id, mmltk::common::system::NumaTopology::Capture(),
                                                  config.loading.numa_node, config.cpu_affinity);
    const auto& cpus = execution.placement.cpus;
    const int requested = config.gather_workers > 0 ? config.gather_workers : config.prefetch_factor;
    const auto workers = static_cast<size_t>(clamp_worker_count_to_cpus(std::min(requested, config.prefetch_factor), cpus.size(), 1, 1));
    state.slots.resize(static_cast<size_t>(config.prefetch_factor));
    state.stream = std::make_unique<CompiledImageStream>(CompiledImageStream::Config{.slots = state.slots.size(),
                                                                                     .workers = workers,
                                                                                     .device = config.device_id,
                                                                                     .cpu_affinity = config.cpu_affinity,
                                                                                     .loading = config.loading,
                                                                                     .execution = execution,
                                                                                     .record_consumer = record_consumer}, std::move(retirement));
    const auto stride = static_cast<size_t>(state.source.header().image_stride);
    if (config.batch_size > std::numeric_limits<size_t>::max() / stride) throw std::overflow_error("dataset batch storage size overflow");
    mmltk::frameworks::gpu::CudaDeviceScope scope(config.device_id);
    if (!scope) { mmltk::frameworks::gpu::ensure_cuda_ok(scope.Finalize(), "dataset loader device binding"); }
    try {
        mmltk::frameworks::gpu::ensure_cuda_ok(cudaFree(nullptr), "dataset loader context initialization");
        state.stream->bind_current_context();
        for (size_t index = 0; index < state.slots.size(); ++index) {
            if (config.loading.h2d_dataloader) state.stream->prepare_host(index, config.batch_size * stride);
            state.stream->prepare_device(index, config.batch_size * stride);
            state.slots[index].reads.reserve(config.batch_size);
        }
    } catch (...) {
        const auto error = std::current_exception();
        mmltk::frameworks::gpu::ensure_cuda_ok(scope.Finalize(), "dataset loader caller restoration");
        std::rethrow_exception(error);
    }
    mmltk::frameworks::gpu::ensure_cuda_ok(scope.Finalize(), "dataset loader caller restoration");
}
void DatasetLoader::stop_workers() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->changed.notify_all();
    impl_->stream->stop_workers();
}

DatasetLoader::~DatasetLoader() {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->checked_out()) std::terminate();
        impl_->stopping = true;
    }
    impl_->changed.notify_all();
    impl_->stream->cancel_reads();
    // Callbacks refer to Impl, so physically destroy the stream before its
    // mutex, source mapping, order, and batch records leave scope.
    impl_->stream.reset();
}
void DatasetLoader::begin_epoch() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->check_failure();
        if (impl_->stopping) throw std::runtime_error("begin_epoch cannot restart a stopped dataset loader");
        if (impl_->checked_out()) throw std::runtime_error("begin_epoch requires all checked-out batches to be released");
        impl_->resetting = true;
    }
    impl_->stream->cancel_reads();
    impl_->stream->synchronize();
    std::lock_guard lock(impl_->mutex);
    impl_->check_failure();
    if (impl_->config.shuffle) {
        std::mt19937_64 rng(impl_->config.seed++);
        build_block_shuffled_order(impl_->order, impl_->source.header().num_images, impl_->config.batch_size, rng, impl_->shuffled_blocks,
                                   impl_->shuffled_chunks);
    }
    impl_->rebuild_schedule();
    impl_->submitted = impl_->consumed = 0;
    for (auto& slot : impl_->slots)
        slot.state = Impl::State::Free;
    impl_->epoch = true;
    impl_->resetting = false;
    for (size_t index = 0; index < impl_->slots.size(); ++index)
        impl_->refill(index);
}
bool DatasetLoader::next_batch(Batch& out) { return next_batch(out, {}); }
bool DatasetLoader::next_batch(Batch& out, std::stop_token stop) {
    if (stop.stop_requested()) return false;
    const auto wake = [this] {
        std::lock_guard lock(impl_->mutex);
        impl_->changed.notify_all();
    };
    std::optional<std::stop_callback<decltype(wake)>> cancellation;
    if (stop.stop_possible()) cancellation.emplace(stop, wake);
    { std::lock_guard lock(impl_->mutex); if (impl_->stopping) return false; }
    if (!impl_->epoch) begin_epoch();
    std::unique_lock lock(impl_->mutex);
    for (;;) {
        if (impl_->stopping) return false;
        impl_->check_failure();
        if (stop.stop_requested()) return false;
        if (impl_->consumed == impl_->batch_starts.size()) return false;
        if (impl_->consumed == impl_->submitted) {
            impl_->changed.wait(lock);
            continue;
        }
        const auto index = impl_->batch_slots[impl_->consumed];
        auto& slot = impl_->slots[index];
        if (slot.batch == impl_->consumed && slot.state == Impl::State::Ready) {
            slot.state = Impl::State::CheckedOut;
            ++impl_->consumed;
            out = {.num_images = slot.count,
                   .device_images = static_cast<const float*>(impl_->stream->device_storage(index).data()),
                   .label_index = impl_->source.label_index().data(),
                   .labels = impl_->source.labels().data(),
                   .rle_pairs = impl_->source.rle_pairs().data(),
                   .image_indices = impl_->order.data() + slot.start,
                   .slot_index = index,
                   .lease_id = slot.lease,
                   .owner = impl_.get()};
            return true;
        }
        impl_->changed.wait(lock);
    }
}
std::span<const float> DatasetLoader::host_images(const Batch& batch) {
    std::lock_guard lock(impl_->mutex);
    auto& slot = impl_->require(batch, "host_images");
    if (!slot.host) slot.host = reinterpret_cast<const float*>(impl_->stream->host_images(batch.slot_index).data());
    return {slot.host, slot.count * image_stride() / sizeof(float)};
}
void DatasetLoader::wait_batch(const Batch& batch) {
    std::lock_guard lock(impl_->mutex);
    impl_->require(batch, "wait_batch", true);
    impl_->stream->wait_transfer(batch.slot_index);
}
void DatasetLoader::handoff_batch(const Batch& batch, void* stream) {
    std::lock_guard lock(impl_->mutex);
    impl_->require(batch, "handoff_batch");
    impl_->stream->handoff(batch.slot_index, stream);
}
void DatasetLoader::release_batch(const Batch& batch) { impl_->release(batch, nullptr, false); }
void DatasetLoader::release_batch(const Batch& batch, void* stream) { impl_->release(batch, stream, true); }
void DatasetLoader::synchronize() { impl_->stream->synchronize(); }
size_t DatasetLoader::num_images() const { return impl_->source.header().num_images; }
size_t DatasetLoader::num_batches() const { return impl_->batch_starts.size(); }
uint32_t DatasetLoader::image_width() const { return impl_->source.header().image_width; }
uint32_t DatasetLoader::image_height() const { return impl_->source.header().image_height; }
uint32_t DatasetLoader::num_classes() const { return impl_->source.header().num_classes; }
uint32_t DatasetLoader::max_instances_per_image() const { return impl_->source.header().max_instances_per_image; }
const char* DatasetLoader::class_name(uint32_t id) const {
    if (id >= num_classes()) throw std::out_of_range("class id out of range");
    return impl_->source.header().class_names[id].data();
}
size_t DatasetLoader::image_stride() const { return impl_->source.header().image_stride; }
size_t DatasetLoader::num_label_instances() const { return impl_->source.labels().size(); }
size_t DatasetLoader::num_rle_pairs() const { return impl_->source.rle_pairs().size(); }
const float* DatasetLoader::pixel_blob() const { return impl_->source.pixel_blob(); }
const LabelIndexEntry* DatasetLoader::label_index() const { return impl_->source.label_index().data(); }
const PackedInstance* DatasetLoader::label_data() const { return impl_->source.labels().data(); }
const RLEPair* DatasetLoader::rle_data() const { return impl_->source.rle_pairs().data(); }
}  // namespace mmltk::backend::data
