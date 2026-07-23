#include "rfdetr/gpu_augment.h"

#include "rfdetr/gpu_augment_cuda_launch.h"
#include "rfdetr/torch_cuda_utils.h"
#include "profile_utils.h"

#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string>

namespace mmltk::rfdetr {

namespace {

std::mutex& augmentation_trace_mutex() {
    static std::mutex mutex;
    return mutex;
}

void check_cuda(cudaError_t status, const char* context) {
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(context) + ": " + cudaGetErrorString(status));
    }
}

GpuAugmentationGroupLaunchConfig launch_group(const AugmentationGroupConfig& group) {
    return {group.probability, group.min_strength, group.max_strength};
}

GpuAugmentationLaunchConfig launch_config(const GpuAugmentationConfig& config) {
    return {
        config.enabled ? 1 : 0,         launch_group(config.geometry), launch_group(config.resize),
        launch_group(config.color),     launch_group(config.noise),    launch_group(config.blur),
        launch_group(config.occlusion),
    };
}

constexpr float kPi = 3.14159265358979323846F;
constexpr std::uint64_t kGoldenRatio = 0x9e3779b97f4a7c15ULL;

std::uint64_t mix64(std::uint64_t value) {
    value += kGoldenRatio;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

float uniform01(const std::uint64_t key, const std::uint64_t counter) {
    return static_cast<float>((mix64(key + counter * kGoldenRatio) >> 40U) & 0xFFFFFFULL) * (1.0F / 16777216.0F);
}

std::uint64_t image_key(const std::uint64_t seed, const int epoch, const int rank, const std::uint64_t sequence,
                        const std::int64_t image) {
    return mix64(seed ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(epoch)) << 32U) ^
                 (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rank)) * 0xd2b74407b1ce6e93ULL) ^
                 (sequence * 0xca5a826395121157ULL) ^ (static_cast<std::uint64_t>(image) * kGoldenRatio));
}

float sample_strength(const AugmentationGroupConfig& group, const std::uint64_t key, std::uint64_t& counter) {
    return std::fma(group.max_strength - group.min_strength, uniform01(key, counter++), group.min_strength);
}

float clamp01(const float value) {
    return std::clamp(value, 0.0F, 1.0F);
}

void prepare_geometry_plan(AugmentationImagePlan& plan, const GpuAugmentationConfig& config, const std::uint64_t key) {
    std::uint64_t counter = 0;
    const bool flip_x = uniform01(key, counter++) < config.geometry.probability * 0.5F;
    const bool flip_y = uniform01(key, counter++) < config.geometry.probability * 0.5F;
    float angle = 0.0F;
    if (uniform01(key, counter++) < config.geometry.probability * 0.35F) {
        const float strength = sample_strength(config.geometry, key, counter);
        angle = (uniform01(key, counter++) * 2.0F - 1.0F) * (10.0F * kPi / 180.0F) * strength;
    }

    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const float rotation_scale = 1.0F / (std::abs(cosine) + std::abs(sine));
    const float flip_scale_x = flip_x ? -1.0F : 1.0F;
    const float flip_scale_y = flip_y ? -1.0F : 1.0F;
    float forward00 = rotation_scale * cosine * flip_scale_x;
    float forward01 = -rotation_scale * sine * flip_scale_y;
    float forward10 = rotation_scale * sine * flip_scale_x;
    float forward11 = rotation_scale * cosine * flip_scale_y;
    float forward02 = 0.5F - 0.5F * (forward00 + forward01);
    float forward12 = 0.5F - 0.5F * (forward10 + forward11);

    plan.resize_scale = 1.0F;
    plan.resize_offset_x = 0.0F;
    plan.resize_offset_y = 0.0F;
    if (uniform01(key, counter++) < config.resize.probability) {
        const float strength = sample_strength(config.resize, key, counter);
        plan.resize_scale = 1.0F + (uniform01(key, counter++) < 0.5F ? -0.5F : 0.5F) * strength;
        const float extent = std::abs(1.0F - plan.resize_scale);
        const float direction = plan.resize_scale < 1.0F ? 1.0F : -1.0F;
        plan.resize_offset_x = uniform01(key, counter++) * extent * direction;
        plan.resize_offset_y = uniform01(key, counter++) * extent * direction;
        forward00 *= plan.resize_scale;
        forward01 *= plan.resize_scale;
        forward02 = std::fma(plan.resize_scale, forward02, plan.resize_offset_x);
        forward10 *= plan.resize_scale;
        forward11 *= plan.resize_scale;
        forward12 = std::fma(plan.resize_scale, forward12, plan.resize_offset_y);
    }

    const float determinant = forward00 * forward11 - forward01 * forward10;
    const float inverse_determinant = 1.0F / determinant;
    const float inverse00 = forward11 * inverse_determinant;
    const float inverse01 = -forward01 * inverse_determinant;
    const float inverse10 = -forward10 * inverse_determinant;
    const float inverse11 = forward00 * inverse_determinant;
    plan.forward = {forward00, forward01, forward02, forward10, forward11, forward12};
    plan.inverse = {inverse00, inverse01, -(inverse00 * forward02 + inverse01 * forward12),
                    inverse10, inverse11, -(inverse10 * forward02 + inverse11 * forward12)};
    plan.area_scale = std::abs(determinant);
}

GpuPreprocessOutputType preprocess_output_type(const at::ScalarType output_type) {
    switch (output_type) {
        case at::kFloat:
            return GpuPreprocessOutputType::Float32;
        case at::kHalf:
            return GpuPreprocessOutputType::Float16;
        case at::kBFloat16:
            return GpuPreprocessOutputType::BFloat16;
        default:
            TORCH_CHECK(false, "GPU batch preprocessing supports only FP32, FP16, and BF16 output");
    }
    return GpuPreprocessOutputType::Float32;
}

}  

GpuBatchPreprocessor::GpuBatchPreprocessor(const std::int64_t batch_capacity, const int height, const int width,
                                           const int device_id, const at::ScalarType output_type)
    : batch_capacity_(batch_capacity),
      height_(height),
      width_(width),
      device_id_(device_id),
      output_type_(output_type) {
    TORCH_CHECK(batch_capacity_ > 0 && height_ > 0 && width_ > 0, "invalid GPU preprocessing tensor shape");
    (void)preprocess_output_type(output_type_);
    c10::cuda::CUDAGuard device_guard(checked_device_index(device_id_));
    output_ = torch::empty({batch_capacity_, 3, height_, width_},
                           torch::TensorOptions().dtype(output_type_).device(cuda_device(device_id_)));
    check_cuda(cudaEventCreateWithFlags(&consumer_complete_, cudaEventDisableTiming),
               "cudaEventCreateWithFlags for GPU preprocessing consumer");
}

GpuBatchPreprocessor::~GpuBatchPreprocessor() {
    if (consumer_complete_ != nullptr) {
        int previous_device = -1;
        const bool restore_device = cudaGetDevice(&previous_device) == cudaSuccess && previous_device != device_id_ &&
                                    cudaSetDevice(device_id_) == cudaSuccess;
        if (consumer_pending_) {
            (void)cudaEventSynchronize(consumer_complete_);
        } else if (has_run_) {
            (void)cudaDeviceSynchronize();
        }
        cudaEventDestroy(consumer_complete_);
        if (restore_device) {
            (void)cudaSetDevice(previous_device);
        }
        consumer_complete_ = nullptr;
    }
}

torch::Tensor GpuBatchPreprocessor::run(const mmltk::Batch& batch, std::int64_t output_batch_size) {
    const auto active_batch_size = static_cast<std::int64_t>(batch.num_images);
    if (output_batch_size == 0) {
        output_batch_size = active_batch_size;
    }
    TORCH_CHECK(active_batch_size > 0 && active_batch_size <= output_batch_size,
                "GPU preprocessing requires a non-empty active batch within the output batch");
    TORCH_CHECK(output_batch_size <= batch_capacity_, "GPU preprocessing batch exceeds preallocated capacity");
    TORCH_CHECK(batch.device_images != nullptr, "GPU preprocessing requires loader device images");

    const auto device_index = checked_device_index(device_id_);
    c10::cuda::CUDAGuard device_guard(device_index);
    const cudaStream_t stream = at::cuda::getCurrentCUDAStream(device_index).stream();
    if (consumer_pending_) {
        check_cuda(cudaStreamWaitEvent(stream, consumer_complete_, 0),
                   "cudaStreamWaitEvent for GPU preprocessing buffer reuse");
        consumer_pending_ = false;
    }
    launch_gpu_batch_normalization(batch.device_images, output_.data_ptr(), active_batch_size, output_batch_size,
                                   height_, width_, preprocess_output_type(output_type_), stream);
    has_run_ = true;
    return output_batch_size == batch_capacity_ ? output_ : output_.narrow(0, 0, output_batch_size);
}

void GpuBatchPreprocessor::record_consumer(cudaStream_t stream) {
    c10::cuda::CUDAGuard device_guard(checked_device_index(device_id_));
    check_cuda(cudaEventRecord(consumer_complete_, stream), "cudaEventRecord for GPU preprocessing consumer");
    consumer_pending_ = true;
}

GpuBatchAugmenter::GpuBatchAugmenter(const GpuAugmentationConfig& config, const std::int64_t batch_capacity,
                                     const int height, const int width, const int device_id, const bool include_masks)
    : config_(config),
      batch_capacity_(batch_capacity),
      height_(height),
      width_(width),
      device_id_(device_id),
      remap_(config.enabled && ((config.geometry.probability > 0.0F && config.geometry.max_strength > 0.0F) ||
                                (config.resize.probability > 0.0F && config.resize.max_strength > 0.0F) ||
                                (config.blur.probability > 0.0F && config.blur.max_strength > 0.0F) ||
                                config.copy_paste_probability > 0.0F)),
      transforms_geometry_(config.enabled && (config.geometry.probability > 0.0F || config.resize.probability > 0.0F)),
      include_masks_(include_masks),
      copy_paste_enabled_(config.enabled && config.copy_paste_probability > 0.0F) {
    TORCH_CHECK(valid_gpu_augmentation_config(config_), "invalid GPU augmentation configuration");
    TORCH_CHECK(batch_capacity_ > 0 && height_ > 0 && width_ > 0, "invalid GPU augmentation tensor shape");
    c10::cuda::CUDAGuard device_guard(checked_device_index(device_id_));
    const auto float_options = torch::TensorOptions().dtype(torch::kFloat32).device(cuda_device(device_id_));
    output_ = torch::empty({batch_capacity_, 3, height_, width_}, float_options);
    parameters_ = torch::empty({batch_capacity_, kGpuAugmentationParameterCount}, float_options);
    batch_plan_.images.resize(static_cast<std::size_t>(batch_capacity_));
    donor_metadata_.resize(static_cast<std::size_t>(batch_capacity_));

    if (copy_paste_enabled_) {
        const auto pinned_float = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU).pinned_memory(true);
        const auto pinned_int64 = torch::TensorOptions().dtype(torch::kInt64).device(torch::kCPU).pinned_memory(true);
        const auto int64_options = torch::TensorOptions().dtype(torch::kInt64).device(cuda_device(device_id_));
        copy_paste_parameters_cpu_ = torch::empty({batch_capacity_, kGpuCopyPasteParameterCount}, pinned_float);
        copy_paste_parameters_gpu_ = torch::empty({batch_capacity_, kGpuCopyPasteParameterCount}, float_options);
        donor_images_ = torch::empty({batch_capacity_, 3, height_, width_}, float_options);
        donor_boxes_cpu_ = torch::empty({batch_capacity_, 4}, pinned_float);
        donor_boxes_gpu_ = torch::empty({batch_capacity_, 4}, float_options);
        replacement_indices_cpu_ = torch::empty({batch_capacity_}, pinned_int64);
        replacement_indices_gpu_ = torch::empty({batch_capacity_}, int64_options);
        paste_donor_slots_cpu_ = torch::empty({batch_capacity_}, pinned_int64);
        paste_donor_slots_gpu_ = torch::empty({batch_capacity_}, int64_options);
        paste_target_indices_cpu_ = torch::empty({batch_capacity_}, pinned_int64);
        paste_target_indices_gpu_ = torch::empty({batch_capacity_}, int64_options);
        mask_words_ = std::max<std::int64_t>(1, (static_cast<std::int64_t>(height_) * width_ + 63) / 64);
        if (include_masks_) {
            donor_masks_ = torch::zeros({batch_capacity_, mask_words_}, int64_options);
        }
        int least_priority = 0;
        int greatest_priority = 0;
        check_cuda(cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority),
                   "cudaDeviceGetStreamPriorityRange for donor cache");
        (void)greatest_priority;
        check_cuda(cudaStreamCreateWithPriority(&cache_stream_, cudaStreamNonBlocking, least_priority),
                   "cudaStreamCreateWithPriority for donor cache");
        check_cuda(cudaEventCreateWithFlags(&image_read_complete_, cudaEventDisableTiming),
                   "cudaEventCreateWithFlags for augmentation image read");
        check_cuda(cudaEventCreateWithFlags(&cache_ready_, cudaEventDisableTiming),
                   "cudaEventCreateWithFlags for donor cache readiness");
    }

    if (const char* path = std::getenv("MMLTK_AUGMENT_TRACE_FILE"); path != nullptr && *path != '\0') {
        trace_fd_ = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (trace_fd_ < 0) {
            throw std::runtime_error(std::string("failed to open MMLTK_AUGMENT_TRACE_FILE: ") + std::strerror(errno));
        }
    }
}

GpuBatchAugmenter::~GpuBatchAugmenter() {
    if (cache_stream_ != nullptr) {
        (void)cudaStreamSynchronize(cache_stream_);
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
    if (trace_fd_ >= 0) {
        ::close(trace_fd_);
        trace_fd_ = -1;
    }
}

void GpuBatchAugmenter::prepare_batch_plan(const mmltk::Batch& batch, const std::uint64_t seed, const int epoch,
                                           const int rank, const std::uint64_t sequence) {
    MMLTK_PROFILE_SCOPE("rfdetr.augment.plan");
    batch_plan_.active_size = batch.num_images;
    batch_plan_.transforms_geometry = transforms_geometry_;
    batch_plan_.copy_paste_enabled = copy_paste_enabled_;
    batch_plan_.include_masks = include_masks_;
    float* paste_parameters = copy_paste_enabled_ ? copy_paste_parameters_cpu_.data_ptr<float>() : nullptr;
    std::int64_t paste_count = 0;
    std::int64_t resized_count = 0;

    for (std::size_t image = 0; image < batch.num_images; ++image) {
        auto& plan = batch_plan_.images[image];
        plan = AugmentationImagePlan{};
        const std::uint64_t key = image_key(seed, epoch, rank, sequence, static_cast<std::int64_t>(image));
        if (config_.enabled) {
            prepare_geometry_plan(plan, config_, key);
        }
        resized_count += plan.resize_scale != 1.0F ? 1 : 0;
        plan.cache_choice = uniform01(key, 0x5000ULL);
        plan.cache_source_dataset_index = batch.image_indices[image];

        if (!copy_paste_enabled_) {
            continue;
        }
        float* paste = paste_parameters + image * kGpuCopyPasteParameterCount;
        std::fill_n(paste, kGpuCopyPasteParameterCount, 0.0F);
        paste[0] = -1.0F;
        if (uniform01(key, 0x4000ULL) >= config_.copy_paste_probability) {
            continue;
        }

        const std::size_t start = std::min<std::size_t>(
            static_cast<std::size_t>(uniform01(key, 0x4001ULL) * static_cast<float>(batch_capacity_)),
            static_cast<std::size_t>(batch_capacity_ - 1));
        std::int64_t donor_slot = -1;
        for (std::int64_t probe = 0; probe < batch_capacity_; ++probe) {
            const std::size_t candidate =
                (start + static_cast<std::size_t>(probe)) % static_cast<std::size_t>(batch_capacity_);
            const auto& donor = donor_metadata_[candidate];
            if (donor.valid && donor.dataset_index != batch.image_indices[image]) {
                donor_slot = static_cast<std::int64_t>(candidate);
                break;
            }
        }
        if (donor_slot < 0) {
            continue;
        }

        const auto& donor = donor_metadata_[static_cast<std::size_t>(donor_slot)];
        const float scale = 0.5F + uniform01(key, 0x4002ULL);
        const float destination_x = uniform01(key, 0x4003ULL);
        const float destination_y = uniform01(key, 0x4004ULL);
        const float source_x = (donor.box[0] + donor.box[2]) * 0.5F;
        const float source_y = (donor.box[1] + donor.box[3]) * 0.5F;
        const float translate_x = destination_x - scale * source_x;
        const float translate_y = destination_y - scale * source_y;
        const float inverse_scale = 1.0F / scale;
        const float output_x0 = clamp01(std::fma(scale, donor.box[0], translate_x));
        const float output_y0 = clamp01(std::fma(scale, donor.box[1], translate_y));
        const float output_x1 = clamp01(std::fma(scale, donor.box[2], translate_x));
        const float output_y1 = clamp01(std::fma(scale, donor.box[3], translate_y));
        if (output_x1 <= output_x0 || output_y1 <= output_y0) {
            continue;
        }

        plan.paste_donor_slot = donor_slot;
        ++paste_count;
        plan.paste_label = donor.label;
        plan.paste_source_area = donor.area;
        plan.paste_source_box = donor.box;
        plan.paste_output_box = {output_x0, output_y0, output_x1, output_y1};
        plan.paste_inverse = {inverse_scale, 0.0F,          -translate_x * inverse_scale,
                              0.0F,          inverse_scale, -translate_y * inverse_scale};
        paste[0] = static_cast<float>(donor_slot);
        paste[1] = include_masks_ ? 1.0F : 2.0F;
        std::copy(plan.paste_inverse.begin(), plan.paste_inverse.end(), paste + 2);
    }
    MMLTK_PROFILE_ADD("rfdetr.augment.resize_selected", resized_count);
    MMLTK_PROFILE_ADD("rfdetr.augment.copy_paste_selected", paste_count);
}

torch::Tensor GpuBatchAugmenter::run(const mmltk::Batch& batch, const std::uint64_t seed, const int epoch,
                                     const int rank, const std::uint64_t sequence) {
    TORCH_CHECK(static_cast<std::int64_t>(batch.num_images) <= batch_capacity_,
                "GPU augmentation batch exceeds preallocated capacity");
    current_batch_size_ = static_cast<std::int64_t>(batch.num_images);
    current_epoch_ = epoch;
    current_rank_ = rank;
    current_sequence_ = sequence;
    prepare_batch_plan(batch, seed, epoch, rank, sequence);
    const auto device_index = checked_device_index(device_id_);
    c10::cuda::CUDAGuard device_guard(device_index);
    const cudaStream_t stream = at::cuda::getCurrentCUDAStream(device_index).stream();
    if (cache_ready_pending_) {
        check_cuda(cudaStreamWaitEvent(stream, cache_ready_, 0), "cudaStreamWaitEvent for donor cache readiness");
        cache_ready_pending_ = false;
    }
    const auto launch = launch_config(config_);
    if (copy_paste_enabled_) {
        check_cuda(
            cudaMemcpyAsync(copy_paste_parameters_gpu_.data_ptr<float>(), copy_paste_parameters_cpu_.data_ptr<float>(),
                            static_cast<std::size_t>(current_batch_size_) * kGpuCopyPasteParameterCount * sizeof(float),
                            cudaMemcpyHostToDevice, stream),
            "cudaMemcpyAsync for copy-paste parameters");
    }
    launch_gpu_augmentation_parameters(parameters_.data_ptr<float>(), current_batch_size_, launch, seed, epoch, rank,
                                       sequence, stream);
    launch_gpu_augmentation_images(
        batch.device_images, output_.data_ptr<float>(), parameters_.data_ptr<float>(),
        copy_paste_enabled_ ? copy_paste_parameters_gpu_.data_ptr<float>() : nullptr,
        copy_paste_enabled_ ? donor_images_.data_ptr<float>() : nullptr,
        include_masks_ && donor_masks_.defined() ? donor_masks_.data_ptr<std::int64_t>() : nullptr,
        copy_paste_enabled_ ? donor_boxes_gpu_.data_ptr<float>() : nullptr, include_masks_ ? mask_words_ : 0,
        current_batch_size_, height_, width_, launch, seed, epoch, rank, sequence, remap_, stream);
    if (copy_paste_enabled_) {
        check_cuda(cudaEventRecord(image_read_complete_, stream), "cudaEventRecord for augmentation image read");
    }
    return current_batch_size_ == batch_capacity_ ? output_ : output_.narrow(0, 0, current_batch_size_);
}

cudaStream_t GpuBatchAugmenter::finish_batch(const mmltk::Batch& batch, PreparedTargets& targets,
                                             TargetScratch& scratch) {
    MMLTK_PROFILE_SCOPE("rfdetr.augment.cache_submit");
    const auto device_index = checked_device_index(device_id_);
    const cudaStream_t current_stream = at::cuda::getCurrentCUDAStream(device_index).stream();
    if (!copy_paste_enabled_ || current_batch_size_ == 0) {
        write_trace(current_epoch_, current_rank_, current_sequence_, current_batch_size_);
        return current_stream;
    }
    TORCH_CHECK(cache_stream_ != nullptr && scratch.copy_complete_event != nullptr,
                "copy-paste cache resources are not initialized");
    c10::cuda::CUDAGuard device_guard(checked_device_index(device_id_));
    auto* replacements = replacement_indices_cpu_.data_ptr<std::int64_t>();
    auto* donor_slots = paste_donor_slots_cpu_.data_ptr<std::int64_t>();
    auto* paste_targets = paste_target_indices_cpu_.data_ptr<std::int64_t>();
    std::int64_t replacement_count = 0;
    for (std::int64_t image = 0; image < batch_capacity_; ++image) {
        replacements[image] = -1;
        donor_slots[image] = -1;
        paste_targets[image] = -1;
    }
    for (std::int64_t image = 0; image < current_batch_size_; ++image) {
        auto& plan = batch_plan_.images[static_cast<std::size_t>(image)];
        donor_slots[image] = plan.paste_donor_slot;
        paste_targets[image] = plan.paste_target_index;
        replacements[image] = plan.cache_source_target_index;
        if (plan.cache_source_target_index >= 0) {
            ++replacement_count;
            auto& metadata = donor_metadata_[static_cast<std::size_t>(image)];
            metadata.valid = true;
            metadata.label = plan.cache_source_label;
            metadata.dataset_index = plan.cache_source_dataset_index;
            metadata.area = plan.cache_source_area;
            metadata.box = plan.cache_source_box;
        }
    }
    float* donor_boxes = donor_boxes_cpu_.data_ptr<float>();
    for (std::int64_t slot = 0; slot < batch_capacity_; ++slot) {
        std::copy(donor_metadata_[static_cast<std::size_t>(slot)].box.begin(),
                  donor_metadata_[static_cast<std::size_t>(slot)].box.end(), donor_boxes + slot * 4);
    }

    check_cuda(cudaStreamWaitEvent(cache_stream_, image_read_complete_, 0),
               "cudaStreamWaitEvent for donor image consumption");
    check_cuda(cudaStreamWaitEvent(cache_stream_, scratch.copy_complete_event, 0),
               "cudaStreamWaitEvent for target cache update");
    const std::size_t map_bytes = static_cast<std::size_t>(current_batch_size_) * sizeof(std::int64_t);
    check_cuda(cudaMemcpyAsync(replacement_indices_gpu_.data_ptr<std::int64_t>(), replacements, map_bytes,
                               cudaMemcpyHostToDevice, cache_stream_),
               "cudaMemcpyAsync for donor replacements");
    check_cuda(cudaMemcpyAsync(paste_donor_slots_gpu_.data_ptr<std::int64_t>(), donor_slots, map_bytes,
                               cudaMemcpyHostToDevice, cache_stream_),
               "cudaMemcpyAsync for pasted donor slots");
    check_cuda(cudaMemcpyAsync(paste_target_indices_gpu_.data_ptr<std::int64_t>(), paste_targets, map_bytes,
                               cudaMemcpyHostToDevice, cache_stream_),
               "cudaMemcpyAsync for pasted target indices");

    std::int64_t* target_masks = nullptr;
    if (include_masks_ && targets.packed_masks.has_value()) {
        target_masks = targets.packed_masks->bits.data_ptr<std::int64_t>();
        launch_gpu_copy_cached_masks(
            donor_masks_.data_ptr<std::int64_t>(), target_masks, paste_donor_slots_gpu_.data_ptr<std::int64_t>(),
            paste_target_indices_gpu_.data_ptr<std::int64_t>(), current_batch_size_, mask_words_, cache_stream_);
    }
    launch_gpu_donor_cache_update(batch.device_images, donor_images_.data_ptr<float>(), target_masks,
                                  include_masks_ ? donor_masks_.data_ptr<std::int64_t>() : nullptr,
                                  replacement_indices_gpu_.data_ptr<std::int64_t>(), current_batch_size_,
                                  static_cast<std::int64_t>(height_) * width_, include_masks_ ? mask_words_ : 0,
                                  cache_stream_);
    check_cuda(cudaMemcpyAsync(donor_boxes_gpu_.data_ptr<float>(), donor_boxes,
                               static_cast<std::size_t>(batch_capacity_) * 4 * sizeof(float), cudaMemcpyHostToDevice,
                               cache_stream_),
               "cudaMemcpyAsync for donor boxes");
    check_cuda(cudaEventRecord(cache_ready_, cache_stream_), "cudaEventRecord for donor cache readiness");
    cache_ready_pending_ = true;
    MMLTK_PROFILE_ADD("rfdetr.augment.cache_replacements", replacement_count);
    MMLTK_PROFILE_ADD("rfdetr.augment.cache_image_bytes",
                      replacement_count * 3 * static_cast<std::int64_t>(height_) * width_ * sizeof(float));
    if (include_masks_) {
        MMLTK_PROFILE_ADD("rfdetr.augment.cache_mask_bytes",
                          replacement_count * mask_words_ * static_cast<std::int64_t>(sizeof(std::int64_t)));
    }
    check_cuda(cudaEventRecord(scratch.copy_complete_event, cache_stream_),
               "cudaEventRecord for copy-paste cache update");
    scratch.copy_pending = true;
    write_trace(current_epoch_, current_rank_, current_sequence_, current_batch_size_);
    return cache_stream_;
}

void GpuBatchAugmenter::write_trace(const int epoch, const int rank, const std::uint64_t sequence,
                                    const std::int64_t batch_size) const {
    if (trace_fd_ < 0) {
        return;
    }
    std::string lines;
    lines.reserve(static_cast<std::size_t>(batch_size) * 384U + 256U);
    std::int64_t valid_slots = 0;
    for (const auto& donor : donor_metadata_) {
        valid_slots += donor.valid ? 1 : 0;
    }
    lines += std::format(
        "{{\"event\":\"gpu_augment_launch\",\"device\":{},\"epoch\":{},\"rank\":{},"
        "\"sequence\":{},\"batch\":{},\"height\":{},\"width\":{},\"enabled\":{},"
        "\"remap\":{},\"valid_donor_slots\":{},\"copy_paste_probability\":{}}}\n",
        device_id_, epoch, rank, sequence, batch_size, height_, width_, config_.enabled, remap_, valid_slots,
        config_.copy_paste_probability);
    for (std::int64_t image = 0; image < batch_size; ++image) {
        const auto& plan = batch_plan_.images[static_cast<std::size_t>(image)];
        lines += std::format(
            "{{\"event\":\"gpu_augment_image\",\"device\":{},\"epoch\":{},\"rank\":{},\"sequence\":{},"
            "\"image\":{},\"resize_scale\":{},\"resize_offset_x\":{},\"resize_offset_y\":{},"
            "\"paste_donor_slot\":{},\"paste_label\":{},\"paste_box\":[{},{},{},{}],"
            "\"cache_source_target\":{},\"cache_source_label\":{}}}\n",
            device_id_, epoch, rank, sequence, image, plan.resize_scale, plan.resize_offset_x, plan.resize_offset_y,
            plan.paste_donor_slot, plan.paste_label, plan.paste_output_box[0], plan.paste_output_box[1],
            plan.paste_output_box[2], plan.paste_output_box[3], plan.cache_source_target_index,
            plan.cache_source_label);
    }
    std::lock_guard lock(augmentation_trace_mutex());
    const ssize_t written = ::write(trace_fd_, lines.data(), lines.size());
    if (written < 0 || static_cast<std::size_t>(written) != lines.size()) {
        throw std::runtime_error(std::string("failed to append GPU augmentation trace: ") + std::strerror(errno));
    }
}

}  
