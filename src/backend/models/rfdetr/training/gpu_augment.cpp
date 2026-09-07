#include "src/backend/ml/cuda/numa_host_tensor.h"
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "torch_api.h"
#include "torch_cuda_utils.h"

// CLEANUP-IGNORE: The augmentation implementation declares its own logging and CUDA module dependencies.
import mmltk.common.logging.mmltk_logging;
// CLEANUP-IGNORE: Profiling and CUDA imports are owned directly by this augmentation implementation.
import mmltk.common.logging.profile_utils;

#include "detail/gpu_augment_private.h"
#include "detail/target_builder_private.h"

namespace mmltk::backend::models::rfdetr {

using mmltk::backend::ml::cuda::checked_device_index;
using mmltk::backend::ml::cuda::current_torch_cuda_stream_object;
using mmltk::backend::ml::cuda::TorchCudaDeviceGuard;
using mmltk::frameworks::gpu::ensure_cuda_ok;

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

[[nodiscard]] std::size_t tensor_bytes(const torch_types::Tensor& tensor) noexcept {
    return tensor.defined() ? static_cast<std::size_t>(tensor.numel()) * tensor.element_size() : 0U;
}

GpuPreprocessOutputType preprocess_output_type(const torch_types::ScalarType output_type) {
    switch (output_type) {
        case torch_types::kFloat:
            return GpuPreprocessOutputType::Float32;
        case torch_types::kHalf:
            return GpuPreprocessOutputType::Float16;
        case torch_types::kBFloat16:
            return GpuPreprocessOutputType::BFloat16;
        default:
            require(false,
                    "GPU batch preprocessing supports only FP32, FP16, and "
                    "BF16 output");
    }
    return GpuPreprocessOutputType::Float32;
}

}  // namespace

GpuBatchPreprocessor::GpuBatchPreprocessor(const std::int64_t batch_capacity, const int height, const int width, const int device_id,
                                           const torch_types::ScalarType output_type)
    : batch_capacity_(batch_capacity), height_(height), width_(width), device_id_(device_id), output_type_(output_type) {
    require(batch_capacity_ > 0 && height_ > 0 && width_ > 0, "invalid GPU preprocessing tensor shape");
    (void)preprocess_output_type(output_type_);
    TorchCudaDeviceGuard device_guard(checked_device_index(device_id_));
    output_ = torch_types::empty({batch_capacity_, 3, height_, width_},
                                 torch_types::TensorOptions().dtype(output_type_).device(cuda_device(device_id_)));
    ensure_cuda_ok(cudaEventCreateWithFlags(&consumer_complete_, cudaEventDisableTiming),
                   "cudaEventCreateWithFlags for GPU preprocessing consumer");
}

GpuBatchPreprocessor::~GpuBatchPreprocessor() {
    if (consumer_complete_ != nullptr) {
        int previous_device = -1;
        const bool restore_device =
            cudaGetDevice(&previous_device) == cudaSuccess && previous_device != device_id_ && cudaSetDevice(device_id_) == cudaSuccess;
        if (consumer_pending_) {
            (void)cudaEventSynchronize(consumer_complete_);
        } else if (has_run_) {
            (void)cudaDeviceSynchronize();
        }
        cudaEventDestroy(consumer_complete_);
        if (restore_device) { (void)cudaSetDevice(previous_device); }
        consumer_complete_ = nullptr;
    }
}

torch_types::Tensor GpuBatchPreprocessor::run(const mmltk::backend::data::Batch& batch, std::int64_t output_batch_size) {
    const auto active_batch_size = static_cast<std::int64_t>(batch.num_images);
    if (output_batch_size == 0) { output_batch_size = active_batch_size; }
    require(active_batch_size > 0 && active_batch_size <= output_batch_size,
            "GPU preprocessing requires a non-empty active batch within the output batch");
    require(output_batch_size <= batch_capacity_, "GPU preprocessing batch exceeds preallocated capacity");
    require(batch.device_images != nullptr, "GPU preprocessing requires loader device images");

    const auto device_index = checked_device_index(device_id_);
    TorchCudaDeviceGuard device_guard(device_index);
    const cudaStream_t stream = current_torch_cuda_stream_object(device_index).stream();
    if (consumer_pending_) {
        ensure_cuda_ok(cudaStreamWaitEvent(stream, consumer_complete_, 0), "cudaStreamWaitEvent for GPU preprocessing buffer reuse");
        consumer_pending_ = false;
    }
    normalize_gpu_batch(batch.device_images, output_.data_ptr(), active_batch_size, output_batch_size, height_, width_,
                        preprocess_output_type(output_type_), stream);
    has_run_ = true;
    return output_batch_size == batch_capacity_ ? output_ : output_.narrow(0, 0, output_batch_size);
}

void GpuBatchPreprocessor::record_consumer(cudaStream_t stream) {
    TorchCudaDeviceGuard device_guard(checked_device_index(device_id_));
    ensure_cuda_ok(cudaEventRecord(consumer_complete_, stream), "cudaEventRecord for GPU preprocessing consumer");
    consumer_pending_ = true;
}

GpuBatchAugmenter::GpuBatchAugmenter(const GpuAugmentationConfig& config, const std::int64_t batch_capacity, const int height,
                                     const int width, const int device_id)
    : config_(config), batch_capacity_(batch_capacity), height_(height), width_(width), device_id_(device_id) {
    require(gpu_augmentation_config_valid(config_), "invalid GPU augmentation configuration");
    require(batch_capacity_ > 0 && height_ > 0 && width_ > 0, "invalid GPU augmentation tensor shape");
    TorchCudaDeviceGuard device_guard(checked_device_index(device_id_));
    const auto float_options = torch_types::TensorOptions().dtype(torch_types::kFloat32).device(cuda_device(device_id_));
    output_ = torch_types::empty({batch_capacity_, 3, height_, width_}, float_options);
    batch_plan_.images.resize(static_cast<std::size_t>(batch_capacity_));
    donor_support_.resize(static_cast<std::size_t>(batch_capacity_));
    donor_metadata_.resize(static_cast<std::size_t>(batch_capacity_));
    executor_ = std::make_unique<GpuAugmentationExecutor>(config_, static_cast<std::size_t>(batch_capacity_), height_, width_, device_id_);

    if (executor_->copy_paste_enabled()) { ensure_copy_paste_resources(); }
}

void GpuBatchAugmenter::ensure_copy_paste_resources() {
    if (donor_images_.defined()) { return; }
    TorchCudaDeviceGuard device_guard(checked_device_index(device_id_));
    const auto float_options = torch_types::TensorOptions().dtype(torch_types::kFloat32).device(cuda_device(device_id_));
    const auto int64_options = torch_types::TensorOptions().dtype(torch_types::kInt64).device(cuda_device(device_id_));

    try {
        donor_boxes_cpu_ = mmltk::backend::ml::cuda::numa_empty({batch_capacity_, 4}, torch_types::kFloat32, device_id_);
        donor_boxes_gpu_ = torch_types::empty({batch_capacity_, 4}, float_options);
        replacement_indices_cpu_ = mmltk::backend::ml::cuda::numa_empty({batch_capacity_}, torch_types::kInt64, device_id_);
        replacement_indices_gpu_ = torch_types::empty({batch_capacity_}, int64_options);
        mask_words_ = packed_mask_words_for_shape(height_, width_);
        donor_masks_ = torch_types::empty({batch_capacity_, mask_words_}, int64_options);
        donor_masks_cpu_ = mmltk::backend::ml::cuda::numa_empty({batch_capacity_, mask_words_}, torch_types::kInt64, device_id_);
        int least_priority = 0;
        int greatest_priority = 0;
        ensure_cuda_ok(cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority),
                       "cudaDeviceGetStreamPriorityRange for donor cache");
        (void)greatest_priority;
        ensure_cuda_ok(cudaStreamCreateWithPriority(&cache_stream_, cudaStreamNonBlocking, least_priority),
                       "cudaStreamCreateWithPriority for donor cache");
        ensure_cuda_ok(cudaEventCreateWithFlags(&image_read_complete_, cudaEventDisableTiming),
                       "cudaEventCreateWithFlags for augmentation image read");
        ensure_cuda_ok(cudaEventCreateWithFlags(&cache_ready_, cudaEventDisableTiming),
                       "cudaEventCreateWithFlags for donor cache readiness");
        ensure_cuda_ok(cudaEventCreateWithFlags(&cache_upload_complete_, cudaEventDisableTiming),
                       "cudaEventCreateWithFlags for donor upload staging");
        donor_images_ = torch_types::empty({batch_capacity_, 3, height_, width_}, float_options);
        mmltk::common::logging::trace([&](auto& logger) {
            const std::size_t pinned_host_bytes =
                tensor_bytes(donor_boxes_cpu_) + tensor_bytes(replacement_indices_cpu_) + tensor_bytes(donor_masks_cpu_);
            const std::size_t device_bytes = tensor_bytes(donor_images_) + tensor_bytes(donor_masks_) + tensor_bytes(donor_boxes_gpu_) +
                                             tensor_bytes(replacement_indices_gpu_);
            logger.trace(
                "event=augment.copy_paste_resources_allocated device={} batch_capacity={} height={} width={} "
                "mask_words={} pinned_host_bytes={} device_bytes={}",
                device_id_, batch_capacity_, height_, width_, mask_words_, pinned_host_bytes, device_bytes);
        });
    } catch (...) {
        release_copy_paste_resources();
        donor_images_ = torch_types::Tensor{};
        throw;
    }
}

void GpuBatchAugmenter::reconfigure(const GpuAugmentationConfig& config) {
    require(!batch_run_pending_, "GPU augmentation cannot be reconfigured before the current batch is finished");
    TorchCudaDeviceGuard device_guard(checked_device_index(device_id_));
    if (cache_stream_ != nullptr) {
        ensure_cuda_ok(cudaStreamSynchronize(cache_stream_), "cudaStreamSynchronize before augmentation reconfigure");
    }
    cache_ready_pending_ = false;
    cache_upload_pending_ = false;
    executor_->Reconfigure(config);
    config_ = config;
    if (executor_->copy_paste_enabled()) { ensure_copy_paste_resources(); }
}

GpuBatchAugmenter::~GpuBatchAugmenter() {
    int previous_device = -1;
    const bool restore_device =
        cudaGetDevice(&previous_device) == cudaSuccess && previous_device != device_id_ && cudaSetDevice(device_id_) == cudaSuccess;
    release_copy_paste_resources();
    if (restore_device) { (void)cudaSetDevice(previous_device); }
}

void GpuBatchAugmenter::release_copy_paste_resources() noexcept {
    if (cache_stream_ != nullptr) { (void)cudaStreamSynchronize(cache_stream_); }
    if (cache_upload_complete_ != nullptr) {
        cudaEventDestroy(cache_upload_complete_);
        cache_upload_complete_ = nullptr;
    }
    if (image_read_complete_ != nullptr) {
        cudaEventDestroy(image_read_complete_);
        image_read_complete_ = nullptr;
    }
    if (cache_ready_ != nullptr) {
        cudaEventDestroy(cache_ready_);
        cache_ready_ = nullptr;
    }
    if (cache_stream_ != nullptr) {
        cudaStreamDestroy(cache_stream_);
        cache_stream_ = nullptr;
    }
}

torch_types::Tensor GpuBatchAugmenter::run(const mmltk::backend::data::Batch& batch, const std::uint64_t seed, const int epoch,
                                           const int rank, const std::uint64_t sequence) {
    require(static_cast<std::int64_t>(batch.num_images) <= batch_capacity_, "GPU augmentation batch exceeds preallocated capacity");
    require(batch.num_images == 0U || (batch.device_images != nullptr && batch.image_indices != nullptr),
            "GPU augmentation requires loader device images and identities");
    current_batch_size_ = static_cast<std::int64_t>(batch.num_images);
    current_epoch_ = epoch;
    current_rank_ = rank;
    current_sequence_ = sequence;
    batch_run_pending_ = false;
    cache_consumer_prepared_ = false;

    const auto device_index = checked_device_index(device_id_);
    TorchCudaDeviceGuard device_guard(device_index);
    const cudaStream_t stream = current_torch_cuda_stream_object(device_index).stream();
    if (cache_ready_pending_) {
        ensure_cuda_ok(cudaStreamWaitEvent(stream, cache_ready_, 0), "cudaStreamWaitEvent for donor cache readiness");
        cache_ready_pending_ = false;
    }
    const GpuAugmentationBatchView raw_batch{
        .input = batch.device_images,
        .output = output_.data_ptr<float>(),
        .image_indices = {batch.image_indices, batch.num_images},
        .height = height_,
        .width = width_,
        .output_domain = GpuAugmentationOutputDomain::ModelNormalized,
    };
    const GpuAugmentationDonorBatchView donors{
        .images = executor_->copy_paste_enabled() ? donor_images_.data_ptr<float>() : nullptr,
        .masks = executor_->copy_paste_enabled() && donor_masks_.defined() ? donor_masks_.data_ptr<std::int64_t>() : nullptr,
        .boxes = executor_->copy_paste_enabled() ? donor_boxes_gpu_.data_ptr<float>() : nullptr,
        .mask_words = mask_words_,
        .selection = GpuAugmentationDonorSelection::Cached,
    };
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_augment_plan{"rfdetr.augment.plan"};
        batch_plan_ = executor_->RunTraining(raw_batch, seed, epoch, rank, sequence, donor_metadata_, donors, stream, sequence % 2U);
    }
    for (auto& image : batch_plan_.images) {
        if (image.paste_donor_slot < 0) continue;
        const auto& support = donor_support_[static_cast<std::size_t>(image.paste_donor_slot)];
        image.paste_support = support.data();
        image.paste_support_count = support.size();
    }
#if MMLTK_ENABLE_PROFILING
    std::int64_t paste_count = 0;
    std::int64_t resized_count = 0;
    for (std::size_t image = 0U; image < batch.num_images; ++image) {
        paste_count += batch_plan_.images[image].paste_donor_slot >= 0 ? 1 : 0;
        resized_count += batch_plan_.images[image].resize_scale != 1.0F ? 1 : 0;
    }
    mmltk::common::logging::profile_add_value("rfdetr.augment.resize_selected", resized_count);
    mmltk::common::logging::profile_add_value("rfdetr.augment.copy_paste_selected", paste_count);
#endif
    if (executor_->copy_paste_enabled()) {
        ensure_cuda_ok(cudaEventRecord(image_read_complete_, stream), "cudaEventRecord for augmentation image read");
    }
    batch_run_pending_ = true;
    return current_batch_size_ == batch_capacity_ ? output_ : output_.narrow(0, 0, current_batch_size_);
}

cudaStream_t GpuBatchAugmenter::prepare_batch_consumer() {
    const auto device_index = checked_device_index(device_id_);
    require(batch_run_pending_, "GPU augmentation cache consumer preparation requires a completed run submission");
    if (!executor_->copy_paste_enabled() || current_batch_size_ == 0) { return current_torch_cuda_stream_object(device_index).stream(); }
    require(cache_stream_ != nullptr && image_read_complete_ != nullptr, "copy-paste cache consumer resources are not initialized");
    TorchCudaDeviceGuard device_guard(device_index);
    ensure_cuda_ok(cudaStreamWaitEvent(cache_stream_, image_read_complete_, 0), "cudaStreamWaitEvent for donor image consumption");
    cache_consumer_prepared_ = true;
    return cache_stream_;
}

cudaStream_t GpuBatchAugmenter::finish_batch(const mmltk::backend::data::Batch& batch) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_augment_cache_submit{"rfdetr.augment.cache_submit"};
    require(batch_run_pending_, "GPU augmentation cache publication requires an active batch");
    const auto device_index = checked_device_index(device_id_);
    const cudaStream_t current_stream = current_torch_cuda_stream_object(device_index).stream();
    const auto trace_batch_plan = [&] {
        mmltk::common::logging::trace([&](auto& logger) {
            std::int64_t valid_slots = 0;
            for (const auto& donor : donor_metadata_) {
                valid_slots += donor.label >= 0 ? 1 : 0;
            }
            logger.trace(
                "event=gpu_augment_launch device={} epoch={} rank={} sequence={} batch={} height={} width={} "
                "enabled={} remap={} valid_donor_slots={} copy_paste_probability={}",
                device_id_, current_epoch_, current_rank_, current_sequence_, current_batch_size_, height_, width_, config_.enabled,
                executor_->remaps_pixels(), valid_slots, config_.copy_paste_probability);
            for (std::int64_t image = 0; image < current_batch_size_; ++image) {
                const auto& plan = batch_plan_.images[static_cast<std::size_t>(image)];
                logger.trace(
                    "event=gpu_augment_image device={} epoch={} rank={} sequence={} image={} resize_scale={} "
                    "resize_offset_x={} resize_offset_y={} paste_donor_slot={} paste_label={} "
                    "paste_box=[{},{},{},{}] cache_source_ordinal={} cache_source_label={} paste_masked={} paste_support_runs={}",
                    device_id_, current_epoch_, current_rank_, current_sequence_, image, plan.resize_scale, plan.resize_offset_x,
                    plan.resize_offset_y, plan.paste_donor_slot, plan.paste_label, plan.paste_output_box[0], plan.paste_output_box[1],
                    plan.paste_output_box[2], plan.paste_output_box[3], plan.cache_source_ordinal, plan.cache_source_label,
                    plan.paste_masked, plan.paste_support_count);
            }
        });
    };
    if (!executor_->copy_paste_enabled() || current_batch_size_ == 0) {
        batch_run_pending_ = false;
        trace_batch_plan();
        return current_stream;
    }
    require(cache_stream_ != nullptr, "copy-paste cache resources are not initialized");
    require(cache_consumer_prepared_, "copy-paste cache consumer was not prepared");
    TorchCudaDeviceGuard device_guard(checked_device_index(device_id_));
    try {
        // Reuse pinned upload storage only after its preceding DMA reads complete.
        // Image/cache consumers remain ordered on the owning streams below.
        if (cache_upload_pending_) {
            ensure_cuda_ok(cudaEventSynchronize(cache_upload_complete_), "cudaEventSynchronize for donor upload staging reuse");
            cache_upload_pending_ = false;
        }
        require(batch.num_images == static_cast<std::size_t>(current_batch_size_) && batch.device_images != nullptr,
                "donor cache publication requires the active source batch");
        // Prepare host allocations before publishing replacement metadata or submitting cache updates.
        for (std::int64_t image = 0; image < current_batch_size_; ++image) {
            const auto& plan = batch_plan_.images[static_cast<std::size_t>(image)];
            if (plan.cache_source_ordinal < 0) continue;
            require(batch.label_index != nullptr && batch.image_indices != nullptr && batch.labels != nullptr,
                    "donor cache replacement requires source labels and identities");
            const auto& entry = batch.label_index[batch.image_indices[image]];
            require(plan.cache_source_ordinal < entry.num_instances, "donor cache source ordinal exceeds source labels");
            const auto& instance = batch.labels[entry.label_begin + plan.cache_source_ordinal];
            require(instance.mask_rle_pairs == 0 || batch.rle_pairs != nullptr, "donor cache source mask storage is missing");
            donor_support_[static_cast<std::size_t>(image)].reserve(instance.mask_rle_pairs);
        }
        auto* replacements = replacement_indices_cpu_.data_ptr<std::int64_t>();
        auto* packed_support = donor_masks_cpu_.data_ptr<std::int64_t>();
#if MMLTK_ENABLE_PROFILING
        std::int64_t replacement_count = 0;
#endif
        for (std::int64_t image = 0; image < batch_capacity_; ++image) {
            replacements[image] = -1;
        }
        for (std::int64_t image = 0; image < current_batch_size_; ++image) {
            auto& plan = batch_plan_.images[static_cast<std::size_t>(image)];
            replacements[image] = plan.cache_source_ordinal;
            if (plan.cache_source_ordinal >= 0) {
#if MMLTK_ENABLE_PROFILING
                ++replacement_count;
#endif
                const auto& entry = batch.label_index[batch.image_indices[image]];
                const auto& instance = batch.labels[entry.label_begin + plan.cache_source_ordinal];
                auto& support = donor_support_[static_cast<std::size_t>(image)];
                support.clear();
                if (instance.mask_rle_pairs != 0) {
                    const auto* begin = batch.rle_pairs + instance.mask_rle_offset / sizeof(mmltk::backend::data::RLEPair);
                    support.assign(begin, begin + instance.mask_rle_pairs);
                }
                pack_compiled_rle_pairs(support, {packed_support + image * mask_words_, static_cast<std::size_t>(mask_words_)});
                auto& metadata = donor_metadata_[static_cast<std::size_t>(image)];
                metadata.label = plan.cache_source_label;
                metadata.dataset_index = plan.cache_source_dataset_index;
                metadata.area = plan.cache_source_area;
                metadata.box = plan.cache_source_box;
                metadata.has_mask = instance.mask_rle_pairs != 0;
            }
        }
        float* donor_boxes = donor_boxes_cpu_.data_ptr<float>();
        for (std::int64_t slot = 0; slot < batch_capacity_; ++slot) {
            std::copy(donor_metadata_[static_cast<std::size_t>(slot)].box.begin(),
                      donor_metadata_[static_cast<std::size_t>(slot)].box.end(), donor_boxes + slot * 4);
        }

        const std::size_t map_bytes = static_cast<std::size_t>(current_batch_size_) * sizeof(std::int64_t);
        ensure_cuda_ok(cudaMemcpyAsync(replacement_indices_gpu_.data_ptr<std::int64_t>(), replacements, map_bytes, cudaMemcpyHostToDevice,
                                       cache_stream_),
                       "cudaMemcpyAsync for donor replacements");
        for (std::int64_t first = 0; first < current_batch_size_;) {
            if (replacements[first] < 0) {
                ++first;
                continue;
            }
            auto end = first + 1;
            while (end < current_batch_size_ && replacements[end] >= 0)
                ++end;
            const auto words = static_cast<std::size_t>((end - first) * mask_words_);
            ensure_cuda_ok(
                cudaMemcpyAsync(donor_masks_.data_ptr<std::int64_t>() + first * mask_words_, packed_support + first * mask_words_,
                                words * sizeof(std::int64_t), cudaMemcpyHostToDevice, cache_stream_),
                "cudaMemcpyAsync for original donor support");
            first = end;
        }
        ensure_cuda_ok(
            cudaMemcpyAsync(donor_boxes_gpu_.data_ptr<float>(), donor_boxes, static_cast<std::size_t>(batch_capacity_) * 4 * sizeof(float),
                            cudaMemcpyHostToDevice, cache_stream_),
            "cudaMemcpyAsync for donor boxes");
        ensure_cuda_ok(cudaEventRecord(cache_upload_complete_, cache_stream_), "cudaEventRecord for donor upload staging");
        cache_upload_pending_ = true;
        update_gpu_augmentation_donor_cache(batch.device_images, donor_images_.data_ptr<float>(),
                                            replacement_indices_gpu_.data_ptr<std::int64_t>(), current_batch_size_,
                                            static_cast<std::int64_t>(height_) * width_, cache_stream_);
        ensure_cuda_ok(cudaEventRecord(cache_ready_, cache_stream_), "cudaEventRecord for donor cache readiness");
        cache_ready_pending_ = true;
#if MMLTK_ENABLE_PROFILING
        mmltk::common::logging::profile_add_value("rfdetr.augment.cache_replacements", replacement_count);
        mmltk::common::logging::profile_add_value("rfdetr.augment.cache_image_bytes",
                                                  replacement_count * 3 * static_cast<std::int64_t>(height_) * width_ * sizeof(float));
        mmltk::common::logging::profile_add_value("rfdetr.augment.cache_mask_bytes",
                                                  replacement_count * mask_words_ * static_cast<std::int64_t>(sizeof(std::int64_t)));
#endif
    } catch (...) {
        // A failed publication cannot leave host metadata selecting partially
        // uploaded image/support pairs. Drain submitted reads before returning.
        (void)cudaStreamSynchronize(cache_stream_);
        for (auto& metadata : donor_metadata_)
            metadata.label = -1;
        cache_ready_pending_ = false;
        cache_upload_pending_ = false;
        batch_run_pending_ = false;
        cache_consumer_prepared_ = false;
        throw;
    }
    batch_run_pending_ = false;
    cache_consumer_prepared_ = false;
    trace_batch_plan();
    return cache_stream_;
}

}  // namespace mmltk::backend::models::rfdetr
