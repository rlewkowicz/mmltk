#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/common/math/deterministic_sampling.h"
#include "src/backend/ml/cuda/numa_host_tensor.h"
#include <cuda_runtime.h>
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
#include "src/backend/models/rfdetr/augmentation/annotation_support.h"
#include "src/common/io/noexcept_io.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/cuda_priority.h"
#include <torch/types.h>
#include <torch/serialize.h>
#include "detail/target_builder_private.h"
import mmltk.backend.models.rfdetr.augmentation.augmentation_metadata;
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
import mmltk.backend.ml.cuda.gpu_quiescence;
namespace mmltk::backend::models::rfdetr {
namespace torch_cuda = mmltk::backend::ml::cuda;
using mmltk::frameworks::gpu::ensure_cuda_ok;
namespace {
class StagingSubmissionGuard final {
   public:
    explicit StagingSubmissionGuard(TargetScratch& scratch) : scratch_(&scratch) {}
    ~StagingSubmissionGuard() noexcept {
        if (scratch_ == nullptr) { return; }
        try {
            finish();
        } catch (...) { std::terminate(); }
    }
    void finish() {
        if (scratch_ != nullptr) {
            scratch_->record_pending_copy_on_stream(scratch_->copy_stream_handle());
            scratch_->record_staging_copy_on_stream(scratch_->copy_stream_handle());
            scratch_ = nullptr;
        }
    }

   private:
    TargetScratch* scratch_;
};
class PendingCudaEventPair final {
   public:
    PendingCudaEventPair() = default;
    ~PendingCudaEventPair() noexcept {
        if (copy_complete_ != nullptr) { static_cast<void>(cudaEventDestroy(copy_complete_)); }
        if (consumers_retired_ != nullptr) { static_cast<void>(cudaEventDestroy(consumers_retired_)); }
    }
    PendingCudaEventPair(const PendingCudaEventPair&) = delete;
    PendingCudaEventPair& operator=(const PendingCudaEventPair&) = delete;
    [[nodiscard]] cudaEvent_t* copy_complete_out() noexcept { return &copy_complete_; }
    [[nodiscard]] cudaEvent_t* consumers_retired_out() noexcept { return &consumers_retired_; }
    [[nodiscard]] std::pair<cudaEvent_t, cudaEvent_t> release() noexcept {
        const auto events = std::pair{copy_complete_, consumers_retired_};
        copy_complete_ = nullptr;
        consumers_retired_ = nullptr;
        return events;
    }

   private:
    cudaEvent_t copy_complete_ = nullptr;
    cudaEvent_t consumers_retired_ = nullptr;
};
class PendingCudaEvent final {
   public:
    PendingCudaEvent() = default;
    ~PendingCudaEvent() noexcept {
        if (event_ != nullptr) { static_cast<void>(cudaEventDestroy(event_)); }
    }
    PendingCudaEvent(const PendingCudaEvent&) = delete;
    PendingCudaEvent& operator=(const PendingCudaEvent&) = delete;
    [[nodiscard]] cudaEvent_t* out() noexcept { return &event_; }
    [[nodiscard]] cudaEvent_t get() const noexcept { return event_; }
    void release() noexcept { event_ = nullptr; }

   private:
    cudaEvent_t event_ = nullptr;
};
constexpr int64_t kMaskWordBits = 64;
void require(const bool condition, const std::string_view message) {
    if (!condition) { throw std::runtime_error(std::string(message)); }
}
void log_target_builder_destruction_failure(const std::string_view operation, const std::string_view detail) noexcept {
    try {
        if (mmltk::common::logging::enabled(spdlog::level::err)) {
            mmltk::common::logging::error([&](auto& logger) { logger.error("fatal: {}: {}", operation, detail); });
            return;
        }
    } catch (...) {}
    constexpr std::string_view fallback = "fatal target-builder destruction failure\n";
    mmltk::common::io::write_all_noexcept(STDERR_FILENO, fallback);
}
torch_cuda::TorchCudaStream require_copy_stream(const TargetScratch& scratch) {
    require(scratch.copy_stream_handle() != 0U, "target copy stream not initialized");
    return torch_cuda::external_torch_cuda_stream(scratch.copy_stream_handle(), mmltk::backend::ml::cuda::checked_device_index(scratch.device_id));
}
// Writes an xyxy box into the target accessor row as cxcywh.
template <typename BoxesAccessor, typename BoxXyxy>
void write_cxcywh_box(BoxesAccessor& boxes, const int64_t index, const BoxXyxy& box_xyxy) {
    boxes[index][0] = (box_xyxy[0] + box_xyxy[2]) * 0.5F;
    boxes[index][1] = (box_xyxy[1] + box_xyxy[3]) * 0.5F;
    boxes[index][2] = box_xyxy[2] - box_xyxy[0];
    boxes[index][3] = box_xyxy[3] - box_xyxy[1];
}
void set_packed_mask_range(int64_t* words_data, size_t start, size_t length) {
    auto* words = reinterpret_cast<uint64_t*>(words_data);
    size_t bit_offset = start;
    size_t remaining = length;
    while (remaining > 0) {
        const size_t word_index = bit_offset / static_cast<size_t>(kMaskWordBits);
        const size_t bit_index = bit_offset % static_cast<size_t>(kMaskWordBits);
        const size_t fill_bits = std::min(remaining, static_cast<size_t>(kMaskWordBits) - bit_index);
        const uint64_t full_mask = std::numeric_limits<uint64_t>::max();
        const uint64_t mask = fill_bits == static_cast<size_t>(kMaskWordBits) ? full_mask : ((uint64_t{1} << fill_bits) - 1);
        words[word_index] |= (mask << bit_index);
        bit_offset += fill_bits;
        remaining -= fill_bits;
    }
}
bool reservoir_select(const float choice, const std::int64_t candidate_count, const std::int64_t instance_index) {
    if (candidate_count <= 1) { return true; }
    const std::uint64_t key = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(choice));
    return mmltk::common::math::deterministic_mix64(key ^ (static_cast<std::uint64_t>(instance_index) * 0xd2b74407b1ce6e93ULL)) %
               static_cast<std::uint64_t>(candidate_count) ==
           0;
}
}  // namespace
std::int64_t packed_mask_words_for_shape(const int height, const int width) noexcept {
    const std::int64_t pixels = static_cast<std::int64_t>(height) * static_cast<std::int64_t>(width);
    return std::max<std::int64_t>(1, (pixels + kMaskWordBits - 1) / kMaskWordBits);
}
void pack_compiled_rle_pairs(const std::span<const mmltk::backend::data::RLEPair> pairs, const std::span<std::int64_t> words) {
    std::fill(words.begin(), words.end(), std::int64_t{0});
    for (const mmltk::backend::data::RLEPair& pair : pairs) { set_packed_mask_range(words.data(), pair.start, pair.length); }
}
void BatchStaticTensors::ensure(int64_t batch_size, int height, int width, int target_device_id) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_targets_static_tensors{"rfdetr.targets.static_tensors"};
    const int64_t requested_capacity = std::max(batch_capacity, batch_size);
    const auto requested_device = mmltk::backend::ml::cuda::cuda_device(target_device_id);
    const bool metadata_matches = device_id == target_device_id && image_height == height && image_width == width && batch_capacity >= batch_size;
    const bool sizes_match =
        sizes.defined() && sizes.device() == requested_device && sizes.dim() == 2 && sizes.size(0) >= requested_capacity && sizes.size(1) == 2;
    const bool mask_matches = nested_mask.defined() && nested_mask.device() == requested_device && nested_mask.dim() == 3 &&
                              nested_mask.size(0) >= requested_capacity && nested_mask.size(1) == height && nested_mask.size(2) == width;
    if (metadata_matches && sizes_match && mask_matches) { return; }
    auto replacement_sizes = make_size_tensor(requested_capacity, static_cast<int64_t>(height), static_cast<int64_t>(width), requested_device);
    auto replacement_mask = torch::zeros({requested_capacity, height, width}, torch::TensorOptions().dtype(torch::kBool).device(requested_device));
    static_assert(std::is_nothrow_move_assignable_v<torch::Tensor>);
    sizes = std::move(replacement_sizes);
    nested_mask = std::move(replacement_mask);
    device_id = target_device_id;
    image_height = height;
    image_width = width;
    batch_capacity = requested_capacity;
}
torch::Tensor BatchStaticTensors::sizes_view(int64_t batch_size) const { return sizes.narrow(0, 0, batch_size); }
torch::Tensor BatchStaticTensors::nested_mask_view(int64_t batch_size) const { return nested_mask.narrow(0, 0, batch_size); }
TargetScratch::TargetScratch(const std::size_t staging_depth) : staging_slots_(std::max<std::size_t>(1, staging_depth)) {}
TargetScratch::~TargetScratch() noexcept {
    try {
        release_copy_resources();
    } catch (const std::exception& ex) {
        log_target_builder_destruction_failure("failed to retire target scratch events before destruction", ex.what());
        std::terminate();
    } catch (...) {
        log_target_builder_destruction_failure("failed to retire target scratch events before destruction", "unknown exception");
        std::terminate();
    }
}
void TargetScratch::release_copy_resources() {
    const bool has_resources = copy_stream_ != 0U || copy_complete_event_ != 0U || consumers_retired_event_ != 0U ||
                               std::ranges::any_of(staging_slots_, [](const TargetStagingSlot& slot) { return slot.copy_complete_event != 0U; });
    if (has_resources) {
        require(copy_stream_device_id_ >= 0, "target scratch event owner is not configured");
        torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(copy_stream_device_id_));
        wait_for_pending_copy();
        if (consumers_pending_ && consumers_retired_event_ != 0U) {
            ensure_cuda_ok(cudaEventSynchronize(reinterpret_cast<cudaEvent_t>(consumers_retired_event_)), "cudaEventSynchronize for target consumers");
            consumers_pending_ = false;
        }
        for (auto& slot : staging_slots_) {
            if (slot.copy_complete_event == 0U) { continue; }
            if (slot.copy_pending) {
                ensure_cuda_ok(cudaEventSynchronize(reinterpret_cast<cudaEvent_t>(slot.copy_complete_event)), "cudaEventSynchronize for target staging copy");
                slot.copy_pending = false;
            }
            ensure_cuda_ok(cudaEventDestroy(reinterpret_cast<cudaEvent_t>(slot.copy_complete_event)), "cudaEventDestroy for target staging copy");
            slot.copy_complete_event = 0U;
        }
        if (copy_complete_event_ != 0U) {
            ensure_cuda_ok(cudaEventDestroy(reinterpret_cast<cudaEvent_t>(copy_complete_event_)), "cudaEventDestroy for target copy");
            copy_complete_event_ = 0U;
        }
        if (consumers_retired_event_ != 0U) {
            ensure_cuda_ok(cudaEventDestroy(reinterpret_cast<cudaEvent_t>(consumers_retired_event_)), "cudaEventDestroy for target consumers");
            consumers_retired_event_ = 0U;
        }
    }
    copy_stream_ = 0U;
    copy_stream_device_id_ = -1;
}
void TargetScratch::ensure_batch(size_t batch_size, int height, int width, int target_device_id) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_targets_ensure_batch{"rfdetr.targets.ensure_batch"};
    batch.ensure(static_cast<int64_t>(batch_size), height, width, target_device_id);
    device_id = target_device_id;
    image_height = height;
    image_width = width;
    offsets.resize(batch_size);
    counts.resize(batch_size);
    const auto target_device = mmltk::backend::ml::cuda::cuda_device(target_device_id);
    const int64_t capacity = std::max<int64_t>(batch.batch_capacity, static_cast<int64_t>(batch_size));
    const auto metadata_tensor_matches = [&](const torch::Tensor& tensor) {
        return tensor.defined() && tensor.device() == target_device && tensor.scalar_type() == torch::kInt64 && tensor.dim() == 1 && tensor.size(0) >= capacity;
    };
    if (!metadata_tensor_matches(offsets_gpu) || !metadata_tensor_matches(counts_gpu)) {
        const auto device_int64 = torch::TensorOptions().dtype(torch::kInt64).device(target_device);
        auto replacement_offsets = torch::empty({capacity}, device_int64);
        auto replacement_counts = torch::empty({capacity}, device_int64);
        static_assert(std::is_nothrow_move_assignable_v<torch::Tensor>);
        offsets_gpu = std::move(replacement_offsets);
        counts_gpu = std::move(replacement_counts);
    }
}
void TargetScratch::ensure_instance_capacity(int64_t instances) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_targets_ensure_instances{"rfdetr.targets.ensure_instances"};
    const int64_t requested_capacity = std::max(instance_capacity, instances);
    const auto target_device = mmltk::backend::ml::cuda::cuda_device(device_id);
    const bool tensor_matches = target_indices_gpu.defined() && target_indices_gpu.device() == target_device && target_indices_gpu.dim() == 1 &&
                                target_indices_gpu.size(0) >= requested_capacity;
    if (instance_capacity >= instances && tensor_matches) { return; }
    auto replacement = torch::arange(requested_capacity, torch::TensorOptions().dtype(torch::kInt64).device(target_device));
    static_assert(std::is_nothrow_move_assignable_v<torch::Tensor>);
    target_indices_gpu = std::move(replacement);
    instance_capacity = requested_capacity;
}
void TargetScratch::ensure_packed_mask_capacity(int64_t instances, int height, int width) {
    mask_height = height;
    mask_width = width;
    mask_words_per_instance = packed_mask_words_for_shape(mask_height, mask_width);
    static_cast<void>(instances);
}
TargetStagingSlot& TargetScratch::acquire_staging_slot(const std::size_t batch_size, const int64_t instances, const bool include_masks, const int height,
                                                       const int width) {
    require(!staging_slot_acquired_, "target staging slot is already acquired");
    require(copy_stream_device_id_ >= 0, "target staging slot acquisition requires configured copy resources");
    torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(copy_stream_device_id_));
    for (std::size_t offset = 0; offset < staging_slots_.size(); ++offset) {
        const std::size_t index = (next_staging_slot_ + offset) % staging_slots_.size();
        auto& slot = staging_slots_[index];
        if (slot.copy_pending) {
            const cudaError_t status = cudaEventQuery(reinterpret_cast<cudaEvent_t>(slot.copy_complete_event));
            if (status == cudaErrorNotReady) { continue; }
            ensure_cuda_ok(status, "cudaEventQuery for target staging copy");
            slot.copy_pending = false;
        }
        const int64_t required_batch = static_cast<int64_t>(batch_size);
        const int64_t required_instances = std::max<int64_t>(instances, 1);
        const int64_t batch_capacity = std::max(slot.batch_capacity, required_batch);
        const int64_t slot_instance_capacity = std::max(slot.instance_capacity, required_instances);
        const int64_t mask_words = include_masks ? packed_mask_words_for_shape(height, width) : 0;
        const auto has_vector_capacity = [](const torch::Tensor& tensor, const int64_t capacity) {
            return tensor.defined() && tensor.dim() == 1 && tensor.size(0) >= capacity;
        };
        const auto has_matrix_capacity = [](const torch::Tensor& tensor, const int64_t capacity, const int64_t columns) {
            return tensor.defined() && tensor.dim() == 2 && tensor.size(0) >= capacity && tensor.size(1) == columns;
        };
        const bool replace_batch = slot.batch_capacity < required_batch || !has_vector_capacity(slot.image_ids, batch_capacity) ||
                                   !has_vector_capacity(slot.offsets, batch_capacity) || !has_vector_capacity(slot.counts, batch_capacity);
        const bool replace_instances = slot.instance_capacity < required_instances || !has_matrix_capacity(slot.boxes, slot_instance_capacity, 4) ||
                                       !has_vector_capacity(slot.labels, slot_instance_capacity) || !has_vector_capacity(slot.area, slot_instance_capacity) ||
                                       !has_vector_capacity(slot.iscrowd, slot_instance_capacity) ||
                                       !has_matrix_capacity(slot.inverse_transforms, slot_instance_capacity, 6) ||
                                       !has_vector_capacity(slot.occluder_mask_indices, slot_instance_capacity) ||
                                       !has_matrix_capacity(slot.occluder_inverse_transforms, slot_instance_capacity, 6) ||
                                       (include_masks && !has_matrix_capacity(slot.erasure, slot_instance_capacity, sizeof(AugmentationSpatialErasure)));
        const bool replace_masks = include_masks && (slot.mask_height != height || slot.mask_width != width ||
                                                     !has_matrix_capacity(slot.packed_masks, required_instances, mask_words));
        const bool create_completion_event = slot.copy_complete_event == 0U;
        if (replace_batch || replace_instances || replace_masks || create_completion_event) {
            static_assert(std::is_nothrow_move_assignable_v<TargetStagingSlot>);
            TargetStagingSlot replacement = slot;
            PendingCudaEvent pending_event;
            const auto allocate_staging = [device = copy_stream_device_id_](const at::IntArrayRef shape, const at::ScalarType type) {
                return torch_cuda::numa_empty(shape, type, device);
            };
            if (create_completion_event) {
                ensure_cuda_ok(cudaEventCreateWithFlags(pending_event.out(), cudaEventDisableTiming), "cudaEventCreateWithFlags for target staging copy");
                replacement.copy_complete_event = reinterpret_cast<std::uintptr_t>(pending_event.get());
            }
            if (replace_batch) {
                replacement.image_ids = allocate_staging({batch_capacity}, torch::kInt64);
                replacement.offsets = allocate_staging({batch_capacity}, torch::kInt64);
                replacement.counts = allocate_staging({batch_capacity}, torch::kInt64);
                replacement.batch_capacity = batch_capacity;
            }
            if (replace_instances) {
                replacement.boxes = allocate_staging({slot_instance_capacity, 4}, torch::kFloat32);
                replacement.labels = allocate_staging({slot_instance_capacity}, torch::kInt64);
                replacement.area = allocate_staging({slot_instance_capacity}, torch::kFloat32);
                replacement.iscrowd = allocate_staging({slot_instance_capacity}, torch::kInt64);
                replacement.inverse_transforms = allocate_staging({slot_instance_capacity, 6}, torch::kFloat32);
                replacement.occluder_mask_indices = allocate_staging({slot_instance_capacity}, torch::kInt64);
                replacement.occluder_inverse_transforms = allocate_staging({slot_instance_capacity, 6}, torch::kFloat32);
                if (include_masks) {
                    replacement.erasure = allocate_staging({slot_instance_capacity, static_cast<int64_t>(sizeof(AugmentationSpatialErasure))}, torch::kUInt8);
                }
                replacement.instance_capacity = slot_instance_capacity;
            }
            if (replace_masks) {
                replacement.packed_masks = allocate_staging({required_instances, mask_words}, torch::kInt64).zero_();
                replacement.mask_height = height;
                replacement.mask_width = width;
                replacement.mask_words_per_instance = mask_words;
            }
            slot = std::move(replacement);
            pending_event.release();
        }
        active_staging_slot_ = index;
        next_staging_slot_ = (index + 1) % staging_slots_.size();
        staging_slot_acquired_ = true;
        return slot;
    }
    throw std::runtime_error("RF-DETR target staging capacity is exhausted");
}
void TargetScratch::record_staging_copy_on_stream(const std::uintptr_t stream) {
    require(staging_slot_acquired_ && stream != 0U, "target staging completion requires an acquired slot and CUDA stream");
    require(copy_stream_device_id_ >= 0, "target staging completion requires a configured event owner");
    torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(copy_stream_device_id_));
    auto& slot = staging_slots_[active_staging_slot_];
    ensure_cuda_ok(cudaEventRecord(reinterpret_cast<cudaEvent_t>(slot.copy_complete_event), reinterpret_cast<cudaStream_t>(stream)),
                   "cudaEventRecord for target staging copy");
    slot.copy_pending = true;
    staging_slot_acquired_ = false;
}
void TargetScratch::ensure_copy_resources(int target_device_id) {
    if (copy_stream_ != 0U && copy_stream_device_id_ == target_device_id && copy_complete_event_ != 0U && consumers_retired_event_ != 0U) {
        torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(copy_stream_device_id_));
        if (consumers_pending_) {
            ensure_cuda_ok(cudaStreamWaitEvent(reinterpret_cast<cudaStream_t>(copy_stream_), reinterpret_cast<cudaEvent_t>(consumers_retired_event_), 0),
                           "cudaStreamWaitEvent for target consumer retirement");
            consumers_pending_ = false;
        }
        return;
    }
    release_copy_resources();
    const auto device_index = mmltk::backend::ml::cuda::checked_device_index(target_device_id);
    torch_cuda::TorchCudaDeviceGuard device_guard(device_index);
    const torch_cuda::TorchCudaStream copy_stream =
        torch_cuda::get_priority_cuda_stream(device_index, mmltk::frameworks::gpu::current_cuda_highest_stream_priority());
    PendingCudaEventPair pending_events;
    ensure_cuda_ok(cudaEventCreateWithFlags(pending_events.copy_complete_out(), cudaEventDisableTiming), "cudaEventCreateWithFlags for target copy");
    ensure_cuda_ok(cudaEventCreateWithFlags(pending_events.consumers_retired_out(), cudaEventDisableTiming), "cudaEventCreateWithFlags for target consumers");
    const auto [copy_complete_event, consumers_retired_event] = pending_events.release();
    copy_stream_ = reinterpret_cast<std::uintptr_t>(copy_stream.stream());
    copy_stream_device_id_ = target_device_id;
    copy_complete_event_ = reinterpret_cast<std::uintptr_t>(copy_complete_event);
    consumers_retired_event_ = reinterpret_cast<std::uintptr_t>(consumers_retired_event);
}
void TargetScratch::wait_for_pending_copy() {
    if (!copy_pending_) { return; }
    require(copy_complete_event_ != 0U && copy_stream_device_id_ >= 0, "pending target copy has no configured event owner");
    const auto copy_complete_event = reinterpret_cast<cudaEvent_t>(copy_complete_event_);
    torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(copy_stream_device_id_));
    ensure_cuda_ok(cudaEventSynchronize(copy_complete_event), "cudaEventSynchronize for target copy");
    copy_pending_ = false;
}
void TargetScratch::handoff_pending_copy_to_current_stream(int target_device_id) const {
    if (!copy_pending_) { return; }
    require(copy_complete_event_ != 0U && copy_stream_device_id_ >= 0, "pending target copy has no configured event owner");
    require(target_device_id == copy_stream_device_id_, "target copy handoff device does not match its event owner");
    const auto device_index = mmltk::backend::ml::cuda::checked_device_index(target_device_id);
    torch_cuda::TorchCudaDeviceGuard device_guard(device_index);
    ensure_cuda_ok(
        cudaStreamWaitEvent(torch_cuda::current_torch_cuda_stream_object(device_index).stream(), reinterpret_cast<cudaEvent_t>(copy_complete_event_), 0),
        "cudaStreamWaitEvent for target copy");
}
void TargetScratch::wait_for_pending_copy_on_stream(const std::uintptr_t stream) const {
    if (!copy_pending_) return;
    require(copy_complete_event_ != 0U && copy_stream_device_id_ >= 0 && stream != 0U, "target copy wait requires configured resources");
    torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(copy_stream_device_id_));
    ensure_cuda_ok(cudaStreamWaitEvent(reinterpret_cast<cudaStream_t>(stream), reinterpret_cast<cudaEvent_t>(copy_complete_event_), 0),
                   "cudaStreamWaitEvent for target copy");
}
void TargetScratch::record_pending_copy_on_stream(const std::uintptr_t stream) {
    require(copy_complete_event_ != 0U && copy_stream_device_id_ >= 0 && stream != 0U, "target copy record requires initialized resources");
    torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(copy_stream_device_id_));
    ensure_cuda_ok(cudaEventRecord(reinterpret_cast<cudaEvent_t>(copy_complete_event_), reinterpret_cast<cudaStream_t>(stream)),
                   "cudaEventRecord for target copy");
    copy_pending_ = true;
}
void TargetScratch::retire_consumers_on_current_stream(const int target_device_id) {
    require(target_device_id == copy_stream_device_id_, "target consumer device does not match its event owner");
    const auto device_index = mmltk::backend::ml::cuda::checked_device_index(target_device_id);
    torch_cuda::TorchCudaDeviceGuard device_guard(device_index);
    retire_consumer_on_stream(reinterpret_cast<std::uintptr_t>(torch_cuda::current_torch_cuda_stream_object(device_index).stream()));
}
void TargetScratch::retire_consumer_on_stream(const std::uintptr_t stream) {
    require(consumers_retired_event_ != 0U && copy_stream_device_id_ >= 0, "target consumer retirement requires initialized resources");
    torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(copy_stream_device_id_));
    const auto consumer_stream = reinterpret_cast<cudaStream_t>(stream);
    if (consumers_pending_) {
        ensure_cuda_ok(cudaStreamWaitEvent(consumer_stream, reinterpret_cast<cudaEvent_t>(consumers_retired_event_), 0),
                       "cudaStreamWaitEvent for prior target consumer");
    }
    const auto device_index = torch_cuda::checked_device_index(copy_stream_device_id_);
    const auto torch_stream = consumer_stream == nullptr ? torch_cuda::current_torch_cuda_stream_object(device_index)
                                                         : torch_cuda::getStreamFromExternal(consumer_stream, device_index);
    batch.sizes.record_stream(torch_stream);
    batch.nested_mask.record_stream(torch_stream);
    offsets_gpu.record_stream(torch_stream);
    counts_gpu.record_stream(torch_stream);
    target_indices_gpu.record_stream(torch_stream);
    ensure_cuda_ok(cudaEventRecord(reinterpret_cast<cudaEvent_t>(consumers_retired_event_), consumer_stream), "cudaEventRecord for target consumer retirement");
    consumers_pending_ = true;
}
TargetConsumerLease::TargetConsumerLease(TargetScratch& scratch, const PreparedTargets& targets, const int device_id)
    : scratch_(&scratch), device_id_(device_id) {
    targets.record_stream(torch_cuda::current_torch_cuda_stream_object(torch_cuda::checked_device_index(device_id_)));
}
TargetConsumerLease::~TargetConsumerLease() noexcept {
    if (scratch_ == nullptr) { return; }
    try {
        retire();
    } catch (const std::exception& ex) {
        log_target_builder_destruction_failure("failed to retire target consumer", ex.what());
        std::terminate();
    } catch (...) {
        log_target_builder_destruction_failure("failed to retire target consumer", "unknown exception");
        std::terminate();
    }
}
void TargetConsumerLease::retire() {
    if (scratch_ == nullptr) { return; }
    scratch_->retire_consumers_on_current_stream(device_id_);
    scratch_ = nullptr;
}
void TargetConsumerLease::handoff() {
    if (scratch_ == nullptr || handed_off_) { return; }
    scratch_->handoff_pending_copy_to_current_stream(device_id_);
    handed_off_ = true;
}
LoaderBatchGuard::LoaderBatchGuard(mmltk::backend::data::DatasetLoader& loader, const mmltk::backend::data::Batch& batch, int device_id)
    : loader_(&loader), batch_(batch), device_id_(device_id) {
    const auto device_index = mmltk::backend::ml::cuda::checked_device_index(device_id_);
    torch_cuda::TorchCudaDeviceGuard device_guard(device_index);
    consumer_stream_ = reinterpret_cast<void*>(torch_cuda::current_torch_cuda_stream_object(device_index).stream());
    loader_->handoff_batch(batch_, consumer_stream_);
}
LoaderBatchGuard::~LoaderBatchGuard() noexcept {
    try {
        release();
    } catch (const std::exception& ex) {
        log_target_builder_destruction_failure("failed to release dataset batch", ex.what());
        std::terminate();
    } catch (...) {
        log_target_builder_destruction_failure("failed to release dataset batch", "unknown exception");
        std::terminate();
    }
}
void LoaderBatchGuard::release() {
    if (loader_ == nullptr) { return; }
    torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(device_id_));
    loader_->release_batch(batch_, consumer_stream_);
    loader_ = nullptr;
}
torch::Tensor make_size_tensor(int64_t batch_size, int64_t image_height, int64_t image_width, const torch::Device& device) {
    auto sizes = torch::empty({batch_size, 2}, torch::TensorOptions().dtype(torch::kInt64).device(device));
    sizes.select(1, 0).fill_(image_height);
    sizes.select(1, 1).fill_(image_width);
    return sizes;
}
torch::Tensor make_device_batch_tensor(const mmltk::backend::data::Batch& batch, int device_id, int64_t image_height, int64_t image_width) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_pybind_device_batch_tensor{"rfdetr.pybind.device_batch_tensor"};
    return torch::from_blob(const_cast<float*>(batch.device_images), {static_cast<int64_t>(batch.num_images), 3, image_height, image_width},
                            torch::TensorOptions().dtype(torch::kFloat32).device(mmltk::backend::ml::cuda::cuda_device(device_id)));
}
void validate_feature_active_target(const int64_t label, const std::span<const float, 4> normalized_cxcywh, const int64_t object_classes) {
    if (object_classes < 1 || label < 0 || label >= object_classes) {
        throw std::runtime_error("feature-active RF-DETR target label is outside the object-class catalog");
    }
    const bool finite_and_normalized = std::isfinite(normalized_cxcywh[0]) && normalized_cxcywh[0] >= 0.0F && normalized_cxcywh[0] <= 1.0F &&
                                       std::isfinite(normalized_cxcywh[1]) && normalized_cxcywh[1] >= 0.0F && normalized_cxcywh[1] <= 1.0F &&
                                       std::isfinite(normalized_cxcywh[2]) && normalized_cxcywh[2] > 0.0F && normalized_cxcywh[2] <= 1.0F &&
                                       std::isfinite(normalized_cxcywh[3]) && normalized_cxcywh[3] > 0.0F && normalized_cxcywh[3] <= 1.0F;
    if (!finite_and_normalized) { throw std::runtime_error("feature-active RF-DETR target box must be finite normalized cxcywh with positive extent"); }
}
PreparedTargets build_targets(const mmltk::backend::data::Batch& batch, int image_height, int image_width, bool include_masks, bool require_masks,
                              int device_id, TargetScratch& scratch, const std::string_view split, const int64_t resolved_query_count,
                              const TrainingSupervisionConfig& supervision, const int64_t object_classes, AugmentationBatchPlan* augmentation_plan) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_targets_total{"rfdetr.targets.total"};
    if (split.empty() || resolved_query_count <= 0) {
        throw std::invalid_argument("RF-DETR target preparation requires a split and positive resolved query count");
    }
    if (augmentation_plan != nullptr && augmentation_plan->active_size != batch.num_images) {
        throw std::runtime_error("augmentation plan batch size does not match target batch");
    }
    if (training_supervision_enabled(supervision) && object_classes < 1) {
        throw std::runtime_error("feature-active RF-DETR supervision requires at least one object class");
    }
    PreparedTargets prepared;
    prepared.split = split;
    prepared.resolved_query_count = resolved_query_count;
    prepared.targets.reserve(batch.num_images);
    scratch.ensure_batch(batch.num_images, image_height, image_width, device_id);
    mmltk::common::logging::profile_add_value("rfdetr.targets.images", batch.num_images);
    const auto device = mmltk::backend::ml::cuda::cuda_device(device_id);
    const auto gpu_float = torch::TensorOptions().dtype(torch::kFloat32).device(device);
    const auto gpu_int64 = torch::TensorOptions().dtype(torch::kInt64).device(device);
    auto& offsets = scratch.offsets;
    auto& counts = scratch.counts;
    int64_t maximum_instances = 0;
    for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
        const auto& entry = batch.label_index[batch.image_indices[image_pos]];
        maximum_instances += static_cast<int64_t>(entry.num_instances);
    }
    if (augmentation_plan != nullptr && augmentation_plan->copy_paste_enabled) { maximum_instances += static_cast<int64_t>(batch.num_images); }
    scratch.ensure_instance_capacity(std::max<int64_t>(maximum_instances, 1));
    if (include_masks) { scratch.ensure_packed_mask_capacity(std::max<int64_t>(maximum_instances, 1), image_height, image_width); }
    scratch.ensure_copy_resources(device_id);
    auto& staging = scratch.acquire_staging_slot(batch.num_images, maximum_instances, include_masks, image_height, image_width);
    StagingSubmissionGuard staging_submission(scratch);
    auto image_ids_cpu = staging.image_ids.narrow(0, 0, static_cast<int64_t>(batch.num_images));
    auto offsets_cpu = staging.offsets.narrow(0, 0, static_cast<int64_t>(batch.num_images));
    auto counts_cpu = staging.counts.narrow(0, 0, static_cast<int64_t>(batch.num_images));
    auto image_ids = image_ids_cpu.accessor<int64_t, 1>();
    auto offset_values = offsets_cpu.accessor<int64_t, 1>();
    auto count_values = counts_cpu.accessor<int64_t, 1>();
    torch::Tensor packed_masks_cpu;
    int64_t* packed_masks_data = nullptr;
    if (include_masks && maximum_instances > 0) {
        packed_masks_cpu = staging.packed_masks.narrow(0, 0, maximum_instances);
        std::memset(packed_masks_cpu.data_ptr<int64_t>(), 0,
                    static_cast<size_t>(maximum_instances) * static_cast<size_t>(staging.mask_words_per_instance) * sizeof(int64_t));
        packed_masks_data = packed_masks_cpu.data_ptr<int64_t>();
    }
    int64_t total_instances = 0;
#if MMLTK_ENABLE_PROFILING
    int64_t mask_instances = 0;
    int64_t dropped_instances = 0;
#endif
    if (maximum_instances > 0) {
        std::memset(staging.iscrowd.data_ptr<int64_t>(), 0, static_cast<size_t>(maximum_instances) * sizeof(int64_t));
        std::fill_n(staging.occluder_mask_indices.data_ptr<int64_t>(), maximum_instances, int64_t{-1});
    }
    auto boxes = staging.boxes.accessor<float, 2>();
    auto labels = staging.labels.accessor<int64_t, 1>();
    auto areas = staging.area.accessor<float, 1>();
    auto inverse_transforms = staging.inverse_transforms.accessor<float, 2>();
    auto occluder_indices = staging.occluder_mask_indices.accessor<int64_t, 1>();
    auto occluder_transforms = staging.occluder_inverse_transforms.accessor<float, 2>();
    const bool erased = include_masks && augmentation_plan != nullptr && augmentation_plan->erases_spatial_support;
    auto* erasure_bytes = erased ? staging.erasure.data_ptr<std::uint8_t>() : nullptr;
    static_assert(std::is_trivially_copyable_v<AugmentationSpatialErasure>);
    const std::array<float, 6> identity{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_targets_pack_instances{"rfdetr.targets.pack_instances"};
        for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
            const uint32_t dataset_index = batch.image_indices[image_pos];
            const auto& entry = batch.label_index[dataset_index];
            auto* image_plan = augmentation_plan != nullptr ? &augmentation_plan->images[image_pos] : nullptr;
            if (image_plan != nullptr) {
                image_plan->cache_source_ordinal = -1;
                image_plan->cache_source_label = -1;
                image_plan->cache_source_area = 0.0F;
            }
            const auto& inverse = image_plan != nullptr ? image_plan->inverse : identity;
            const auto erasure = image_plan != nullptr ? image_plan->erasure : AugmentationSpatialErasure{};
            auto* paste_plan = image_plan;
            if (paste_plan != nullptr && paste_plan->paste_donor_slot < 0) { paste_plan = nullptr; }
            const int64_t image_offset = total_instances;
            int64_t retained_candidates = 0;
            image_ids[static_cast<int64_t>(image_pos)] = static_cast<int64_t>(dataset_index) + 1;
            for (int64_t instance_index = 0; instance_index < static_cast<int64_t>(entry.num_instances); ++instance_index) {
                const auto& instance = batch.labels[static_cast<size_t>(entry.label_begin) + static_cast<size_t>(instance_index)];
                if (require_masks && instance.mask_rle_pairs == 0)
                    throw std::runtime_error("segmentation training requires decodable masks for every instance");
                if (instance.mask_rle_pairs != 0 && batch.rle_pairs == nullptr) throw std::runtime_error("mask_rle storage is missing");
                const auto runs = instance.mask_rle_pairs != 0 ? std::span{batch.rle_pairs + instance.mask_rle_offset / sizeof(mmltk::backend::data::RLEPair),
                                                                           static_cast<std::size_t>(instance.mask_rle_pairs)}
                                                               : std::span<const mmltk::backend::data::RLEPair>{};
                const AugmentationMappedInstance mapped = map_augmentation_instance(instance, image_width, image_height, image_plan, runs);
                if (!mapped.visible) {
#if MMLTK_ENABLE_PROFILING
                    ++dropped_instances;
#endif
                    continue;
                }
                const auto& source_box = mapped.source_box_xyxy;
                const auto& transformed = mapped.output_box_xyxy;
                const int64_t target_index = total_instances++;
                write_cxcywh_box(boxes, target_index, transformed);
                labels[target_index] = static_cast<int64_t>(instance.class_id);
                if (erasure_bytes != nullptr) { std::memcpy(erasure_bytes + target_index * sizeof(erasure), &erasure, sizeof(erasure)); }
                std::copy(inverse.begin(), inverse.end(), &inverse_transforms[target_index][0]);
                std::copy(paste_plan != nullptr ? paste_plan->paste_inverse.begin() : identity.begin(),
                          paste_plan != nullptr ? paste_plan->paste_inverse.end() : identity.end(), &occluder_transforms[target_index][0]);
#if MMLTK_ENABLE_PROFILING
                if (include_masks) ++mask_instances;
#endif
                // Source support is already validated by the native mapper.
                // Donor metadata describes those pixels independently of loss selection.
                float source_area = runs.empty() ? mapped.source_area_pixels : 0.0F;
                for (const auto& pair : runs) {
                    if (include_masks) {
                        int64_t* mask_words = packed_masks_data + static_cast<size_t>(target_index) * static_cast<size_t>(staging.mask_words_per_instance);
                        set_packed_mask_range(mask_words, static_cast<size_t>(pair.start), static_cast<size_t>(pair.length));
                    }
                    source_area += static_cast<float>(pair.length);
                }
                areas[target_index] = mapped.output_area;
                ++retained_candidates;
                if (image_plan != nullptr && reservoir_select(image_plan->cache_choice, retained_candidates, instance_index)) {
                    image_plan->cache_source_ordinal = instance_index;
                    image_plan->cache_source_label = static_cast<int64_t>(instance.class_id);
                    image_plan->cache_source_dataset_index = dataset_index;
                    image_plan->cache_source_area = source_area;
                    image_plan->cache_source_box = source_box;
                }
            }
            const int64_t original_end = total_instances;
            const auto donor_support = paste_plan != nullptr
                                           ? resolve_augmentation_annotation_support(paste_plan->paste_source_box,
                                                                                     std::span{paste_plan->paste_support, paste_plan->paste_support_count},
                                                                                     image_width, image_height, paste_plan, true)
                                           : AugmentationAnnotationSupport{};
            if (paste_plan != nullptr && donor_support.present) {
                const int64_t paste_index = total_instances++;
                const auto& paste_box = donor_support.box_xyxy;
                write_cxcywh_box(boxes, paste_index, paste_box);
                labels[paste_index] = paste_plan->paste_label;
                if (erasure_bytes != nullptr) { std::memcpy(erasure_bytes + paste_index * sizeof(erasure), &erasure, sizeof(erasure)); }
                areas[paste_index] = donor_support.area_pixels;
                std::copy(paste_plan->paste_inverse.begin(), paste_plan->paste_inverse.end(), &inverse_transforms[paste_index][0]);
                std::copy(identity.begin(), identity.end(), &occluder_transforms[paste_index][0]);
                occluder_indices[paste_index] = -1;
                for (int64_t target_index = image_offset; target_index < original_end; ++target_index) { occluder_indices[target_index] = paste_index; }
                if (include_masks) {
                    pack_compiled_rle_pairs(
                        {paste_plan->paste_support, paste_plan->paste_support_count},
                        {packed_masks_data + paste_index * staging.mask_words_per_instance, static_cast<std::size_t>(staging.mask_words_per_instance)});
#if MMLTK_ENABLE_PROFILING
                    ++mask_instances;
#endif
                }
            }
            offsets[image_pos] = image_offset;
            counts[image_pos] = total_instances - image_offset;
            offset_values[static_cast<int64_t>(image_pos)] = image_offset;
            count_values[static_cast<int64_t>(image_pos)] = counts[image_pos];
        }
    }
    if (training_supervision_enabled(supervision)) {
        for (int64_t index = 0; index < total_instances; ++index) {
            validate_feature_active_target(labels[index], std::span<const float, 4>(&boxes[index][0], 4), object_classes);
        }
    }
    mmltk::common::logging::profile_add_value("rfdetr.targets.instances", total_instances);
#if MMLTK_ENABLE_PROFILING
    mmltk::common::logging::profile_add_value("rfdetr.targets.mask_instances", mask_instances);
    mmltk::common::logging::profile_add_value("rfdetr.targets.augmentation_dropped", dropped_instances);
#endif
    torch::Tensor boxes_gpu = torch::zeros({0, 4}, gpu_float);
    torch::Tensor labels_gpu = torch::zeros({0}, gpu_int64);
    torch::Tensor area_gpu = torch::zeros({0}, gpu_float);
    torch::Tensor iscrowd_gpu = torch::zeros({0}, gpu_int64);
    std::optional<PackedTargetMasks> packed_masks_gpu;
    torch::Tensor image_ids_gpu;
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_targets_h2d{"rfdetr.targets.h2d"};
        torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(device_id));
        const auto copy_stream = require_copy_stream(scratch);
        torch_cuda::TorchCudaStreamGuard stream_guard(copy_stream);
        image_ids_gpu = image_ids_cpu.to(device, torch::kInt64, true, false);
        scratch.offsets_gpu.narrow(0, 0, static_cast<int64_t>(batch.num_images)).copy_(offsets_cpu, true);
        scratch.counts_gpu.narrow(0, 0, static_cast<int64_t>(batch.num_images)).copy_(counts_cpu, true);
        if (total_instances > 0) {
            boxes_gpu = staging.boxes.narrow(0, 0, total_instances).to(device, torch::kFloat32, true, false);
            labels_gpu = staging.labels.narrow(0, 0, total_instances).to(device, torch::kInt64, true, false);
            area_gpu = staging.area.narrow(0, 0, total_instances).to(device, torch::kFloat32, true, false);
            iscrowd_gpu = staging.iscrowd.narrow(0, 0, total_instances).to(device, torch::kInt64, true, false);
            if (include_masks) {
                const bool transformed = augmentation_plan != nullptr && (augmentation_plan->transforms_geometry || augmentation_plan->copy_paste_enabled ||
                                                                          augmentation_plan->erases_spatial_support);
                packed_masks_gpu = PackedTargetMasks{
                    staging.packed_masks.narrow(0, 0, total_instances).to(device, torch::kInt64, true, false),
                    image_height,
                    image_width,
                    transformed ? staging.inverse_transforms.narrow(0, 0, total_instances).to(device, torch::kFloat32, true, false) : torch::Tensor{},
                    transformed ? staging.occluder_mask_indices.narrow(0, 0, total_instances).to(device, torch::kInt64, true, false) : torch::Tensor{},
                    transformed ? staging.occluder_inverse_transforms.narrow(0, 0, total_instances).to(device, torch::kFloat32, true, false) : torch::Tensor{},
                    augmentation_plan != nullptr && augmentation_plan->erases_spatial_support
                        ? staging.erasure.narrow(0, 0, total_instances).to(device, torch::kUInt8, true, false)
                        : torch::Tensor{},
                };
            }
        } else if (include_masks) {
            packed_masks_gpu = PackedTargetMasks{
                torch::zeros({0, packed_mask_words_for_shape(image_height, image_width)}, gpu_int64), image_height, image_width, {}, {}, {},
            };
        }
    }
    {
        torch_cuda::TorchCudaDeviceGuard device_guard(mmltk::backend::ml::cuda::checked_device_index(device_id));
        const auto copy_stream = require_copy_stream(scratch);
        torch_cuda::TorchCudaStreamGuard stream_guard(copy_stream);
        staging_submission.finish();
    }
    prepared.all_image_ids = image_ids_gpu;
    prepared.orig_sizes = scratch.batch.sizes_view(static_cast<int64_t>(batch.num_images));
    prepared.nested_mask = scratch.batch.nested_mask_view(static_cast<int64_t>(batch.num_images));
    prepared.all_boxes = boxes_gpu;
    prepared.all_labels = labels_gpu;
    prepared.all_area = area_gpu;
    prepared.all_iscrowd = iscrowd_gpu;
    prepared.target_offsets = scratch.offsets_gpu.narrow(0, 0, static_cast<int64_t>(batch.num_images));
    prepared.target_counts = scratch.counts_gpu.narrow(0, 0, static_cast<int64_t>(batch.num_images));
    prepared.target_indices = total_instances > 0 ? scratch.target_indices_gpu.narrow(0, 0, total_instances) : torch::empty({0}, gpu_int64);
    prepared.packed_masks = std::move(packed_masks_gpu);
    prepared.offsets = scratch.offsets;
    prepared.counts = scratch.counts;
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_targets_pack_views{"rfdetr.targets.pack_views"};
        for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
            PreparedTarget target;
            target.image_id = image_ids_gpu.narrow(0, static_cast<int64_t>(image_pos), 1);
            target.orig_size = prepared.orig_sizes.select(0, static_cast<int64_t>(image_pos));
            target.size = prepared.orig_sizes.select(0, static_cast<int64_t>(image_pos));
            target.boxes = boxes_gpu.narrow(0, offsets[image_pos], counts[image_pos]);
            target.labels = labels_gpu.narrow(0, offsets[image_pos], counts[image_pos]);
            target.area = area_gpu.narrow(0, offsets[image_pos], counts[image_pos]);
            target.iscrowd = iscrowd_gpu.narrow(0, offsets[image_pos], counts[image_pos]);
            prepared.targets.push_back(std::move(target));
        }
    }
    return prepared;
}
}  // namespace mmltk::backend::models::rfdetr
