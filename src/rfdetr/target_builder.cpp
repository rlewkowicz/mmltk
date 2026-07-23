#include "mmltk/rfdetr/target_builder.h"

#include "compiled_format.h"
#include "mmltk_logging.h"
#include "profile_utils.h"
#include "rfdetr/torch_cuda_utils.h"

#include <filesystem>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>

namespace mmltk::rfdetr {

namespace {

constexpr int64_t kMaskWordBits = 64;

void check_cuda_status(cudaError_t status, const char* context) {
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(context) + ": " + cudaGetErrorString(status));
    }
}

int64_t packed_mask_words_for_shape(int height, int width) {
    const int64_t pixels = static_cast<int64_t>(height) * static_cast<int64_t>(width);
    return std::max<int64_t>(1, (pixels + kMaskWordBits - 1) / kMaskWordBits);
}

c10::cuda::CUDAStream require_copy_stream(const TargetScratch& scratch) {
    TORCH_CHECK(scratch.copy_stream.has_value(), "target copy stream not initialized");
    return *scratch.copy_stream;
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
        const uint64_t mask =
            fill_bits == static_cast<size_t>(kMaskWordBits) ? full_mask : ((uint64_t{1} << fill_bits) - 1);
        words[word_index] |= (mask << bit_index);
        bit_offset += fill_bits;
        remaining -= fill_bits;
    }
}

std::array<float, 4> transform_box_xyxy(const std::array<float, 4>& box,
                                        const std::array<float, kAugmentationTransformSize>& transform) {
    float minimum_x = 1.0F;
    float minimum_y = 1.0F;
    float maximum_x = 0.0F;
    float maximum_y = 0.0F;
    for (int corner = 0; corner < 4; ++corner) {
        const float x = (corner & 1) != 0 ? box[2] : box[0];
        const float y = (corner & 2) != 0 ? box[3] : box[1];
        const float transformed_x = transform[0] * x + transform[1] * y + transform[2];
        const float transformed_y = transform[3] * x + transform[4] * y + transform[5];
        minimum_x = std::min(minimum_x, transformed_x);
        minimum_y = std::min(minimum_y, transformed_y);
        maximum_x = std::max(maximum_x, transformed_x);
        maximum_y = std::max(maximum_y, transformed_y);
    }
    return {
        std::clamp(minimum_x, 0.0F, 1.0F),
        std::clamp(minimum_y, 0.0F, 1.0F),
        std::clamp(maximum_x, 0.0F, 1.0F),
        std::clamp(maximum_y, 0.0F, 1.0F),
    };
}

float box_area(const std::array<float, 4>& box) {
    return std::max(0.0F, box[2] - box[0]) * std::max(0.0F, box[3] - box[1]);
}

float box_intersection_area(const std::array<float, 4>& left, const std::array<float, 4>& right) {
    return std::max(0.0F, std::min(left[2], right[2]) - std::max(left[0], right[0])) *
           std::max(0.0F, std::min(left[3], right[3]) - std::max(left[1], right[1]));
}

std::uint64_t mix_cache_choice(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

bool reservoir_select(const float choice, const std::int64_t candidate_count, const std::int64_t instance_index) {
    if (candidate_count <= 1) {
        return true;
    }
    const std::uint64_t key = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(choice));
    return mix_cache_choice(key ^ (static_cast<std::uint64_t>(instance_index) * 0xd2b74407b1ce6e93ULL)) %
               static_cast<std::uint64_t>(candidate_count) ==
           0;
}

}  

c10::DeviceIndex cuda_device_index(int device_id) {
    return checked_device_index(device_id);
}

torch::Device cuda_device(int device_id) {
    return {torch::kCUDA, cuda_device_index(device_id)};
}

void BatchStaticTensors::ensure(int64_t batch_size, int height, int width, int target_device_id) {
    MMLTK_PROFILE_SCOPE("rfdetr.targets.static_tensors");
    const bool stale_shape = device_id != target_device_id || image_height != height || image_width != width;
    if (!stale_shape && batch_capacity >= batch_size && sizes.defined() && nested_mask.defined()) {
        return;
    }

    device_id = target_device_id;
    image_height = height;
    image_width = width;
    batch_capacity = std::max(batch_capacity, batch_size);
    const auto device = cuda_device(device_id);
    sizes =
        make_size_tensor(batch_capacity, static_cast<int64_t>(image_height), static_cast<int64_t>(image_width), device);
    nested_mask = torch::zeros({batch_capacity, image_height, image_width},
                               torch::TensorOptions().dtype(torch::kBool).device(device));
}

torch::Tensor BatchStaticTensors::sizes_view(int64_t batch_size) const {
    return sizes.narrow(0, 0, batch_size);
}

torch::Tensor BatchStaticTensors::nested_mask_view(int64_t batch_size) const {
    return nested_mask.narrow(0, 0, batch_size);
}

TargetScratch::~TargetScratch() {
    try {
        wait_for_pending_copy();
    } catch (const std::exception& ex) {
        mmltk::logging::logger("rfdetr.target_builder")
            ->error("fatal: failed to finish target copy before destruction: {}", ex.what());
        std::terminate();
    } catch (...) {
        mmltk::logging::logger("rfdetr.target_builder")
            ->error("fatal: failed to finish target copy before destruction with unknown exception");
        std::terminate();
    }
    if (copy_complete_event != nullptr) {
        cudaEventDestroy(copy_complete_event);
        copy_complete_event = nullptr;
    }
}

void TargetScratch::ensure_batch(size_t batch_size, int height, int width, int target_device_id) {
    MMLTK_PROFILE_SCOPE("rfdetr.targets.ensure_batch");
    wait_for_pending_copy();
    batch.ensure(static_cast<int64_t>(batch_size), height, width, target_device_id);
    device_id = target_device_id;
    image_height = height;
    image_width = width;
    offsets.resize(batch_size);
    counts.resize(batch_size);
    const auto target_device = cuda_device(target_device_id);
    const bool metadata_buffers_stale = !image_ids_cpu.defined() || !offsets_cpu.defined() || !counts_cpu.defined() ||
                                        !offsets_gpu.defined() || !counts_gpu.defined() ||
                                        offsets_gpu.device() != target_device || counts_gpu.device() != target_device ||
                                        image_ids_cpu.size(0) < static_cast<int64_t>(batch_size);
    if (metadata_buffers_stale) {
        const int64_t capacity = std::max<int64_t>(batch.batch_capacity, static_cast<int64_t>(batch_size));
        const auto pinned_int64 = torch::TensorOptions().dtype(torch::kInt64).device(torch::kCPU).pinned_memory(true);
        image_ids_cpu = torch::empty({capacity}, pinned_int64);
        offsets_cpu = torch::empty({capacity}, pinned_int64);
        counts_cpu = torch::empty({capacity}, pinned_int64);
        const auto device_int64 = torch::TensorOptions().dtype(torch::kInt64).device(target_device);
        offsets_gpu = torch::empty({capacity}, device_int64);
        counts_gpu = torch::empty({capacity}, device_int64);
    }
}

void TargetScratch::ensure_instance_capacity(int64_t instances) {
    MMLTK_PROFILE_SCOPE("rfdetr.targets.ensure_instances");
    const bool tensors_ready = boxes_cpu.defined() && labels_cpu.defined() && area_cpu.defined() &&
                               iscrowd_cpu.defined() && inverse_transforms_cpu.defined() &&
                               occluder_mask_indices_cpu.defined() && occluder_inverse_transforms_cpu.defined() &&
                               boxes_cpu.size(1) == 4 && target_indices_gpu.defined() &&
                               target_indices_gpu.device() == cuda_device(device_id);
    if (instance_capacity >= instances && tensors_ready) {
        return;
    }

    instance_capacity = std::max(instance_capacity, instances);
    const auto cpu_float = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU).pinned_memory(true);
    const auto cpu_int64 = torch::TensorOptions().dtype(torch::kInt64).device(torch::kCPU).pinned_memory(true);

    boxes_cpu = torch::empty({instance_capacity, 4}, cpu_float);
    labels_cpu = torch::empty({instance_capacity}, cpu_int64);
    area_cpu = torch::empty({instance_capacity}, cpu_float);
    iscrowd_cpu = torch::empty({instance_capacity}, cpu_int64);
    inverse_transforms_cpu = torch::empty({instance_capacity, 6}, cpu_float);
    occluder_mask_indices_cpu = torch::empty({instance_capacity}, cpu_int64);
    occluder_inverse_transforms_cpu = torch::empty({instance_capacity, 6}, cpu_float);
    target_indices_gpu =
        torch::arange(instance_capacity, torch::TensorOptions().dtype(torch::kInt64).device(cuda_device(device_id)));
}

void TargetScratch::ensure_packed_mask_capacity(int64_t instances, int height, int width) {
    const bool shape_matches = mask_height == height && mask_width == width;
    if (packed_masks_cpu.defined() && shape_matches && packed_masks_cpu.size(0) >= instances) {
        return;
    }

    mask_height = height;
    mask_width = width;
    mask_words_per_instance = packed_mask_words_for_shape(mask_height, mask_width);
    const int64_t mask_capacity = std::max<int64_t>(instances, 1);
    packed_masks_cpu =
        torch::zeros({mask_capacity, mask_words_per_instance},
                     torch::TensorOptions().dtype(torch::kInt64).device(torch::kCPU).pinned_memory(true));
}

void TargetScratch::ensure_copy_resources(int target_device_id) {
    if (copy_stream.has_value() && copy_stream_device_id == target_device_id && copy_complete_event != nullptr) {
        return;
    }
    wait_for_pending_copy();
    if (copy_complete_event != nullptr) {
        cudaEventDestroy(copy_complete_event);
        copy_complete_event = nullptr;
    }
    const auto device_index = cuda_device_index(target_device_id);
    c10::cuda::CUDAGuard device_guard(device_index);
    copy_stream = get_high_priority_cuda_stream(device_index);
    copy_stream_device_id = target_device_id;
    check_cuda_status(cudaEventCreateWithFlags(&copy_complete_event, cudaEventDisableTiming),
                      "cudaEventCreateWithFlags for target copy");
}

void TargetScratch::wait_for_pending_copy() {
    if (!copy_pending || copy_complete_event == nullptr) {
        return;
    }
    if (device_id >= 0) {
        c10::cuda::CUDAGuard device_guard(cuda_device_index(device_id));
        check_cuda_status(cudaEventSynchronize(copy_complete_event), "cudaEventSynchronize for target copy");
    } else {
        check_cuda_status(cudaEventSynchronize(copy_complete_event), "cudaEventSynchronize for target copy");
    }
    copy_pending = false;
}

void TargetScratch::handoff_pending_copy_to_current_stream(int target_device_id) const {
    if (!copy_pending || copy_complete_event == nullptr) {
        return;
    }
    const auto device_index = cuda_device_index(target_device_id);
    c10::cuda::CUDAGuard device_guard(device_index);
    check_cuda_status(
        cudaStreamWaitEvent(c10::cuda::getCurrentCUDAStream(device_index).stream(), copy_complete_event, 0),
        "cudaStreamWaitEvent for target copy");
}

LoaderBatchGuard::LoaderBatchGuard(mmltk::DatasetLoader& loader, const mmltk::Batch& batch, int device_id)
    : loader_(&loader), batch_(batch), device_id_(device_id) {
    const auto device_index = cuda_device_index(device_id_);
    c10::cuda::CUDAGuard device_guard(device_index);
    consumer_stream_ = reinterpret_cast<void*>(c10::cuda::getCurrentCUDAStream(device_index).stream());
    loader_->handoff_batch(batch_, consumer_stream_);
}

LoaderBatchGuard::~LoaderBatchGuard() {
    try {
        release();
    } catch (const std::exception& ex) {
        mmltk::logging::logger("rfdetr.target_builder")->error("fatal: failed to release dataset batch: {}", ex.what());
        std::terminate();
    } catch (...) {
        mmltk::logging::logger("rfdetr.target_builder")
            ->error("fatal: failed to release dataset batch with unknown exception");
        std::terminate();
    }
}

void LoaderBatchGuard::release() {
    if (loader_ == nullptr) {
        return;
    }
    c10::cuda::CUDAGuard device_guard(cuda_device_index(device_id_));
    loader_->release_batch(batch_, consumer_stream_);
    loader_ = nullptr;
}

torch::Tensor make_size_tensor(int64_t batch_size, int64_t image_height, int64_t image_width,
                               const torch::Device& device) {
    auto sizes = torch::empty({batch_size, 2}, torch::TensorOptions().dtype(torch::kInt64).device(device));
    sizes.select(1, 0).fill_(image_height);
    sizes.select(1, 1).fill_(image_width);
    return sizes;
}

torch::Tensor make_device_batch_tensor(const mmltk::Batch& batch, int device_id, int64_t image_height,
                                       int64_t image_width) {
    MMLTK_PROFILE_SCOPE("rfdetr.pybind.device_batch_tensor");
    return torch::from_blob(const_cast<float*>(batch.device_images),
                            {static_cast<int64_t>(batch.num_images), 3, image_height, image_width},
                            torch::TensorOptions().dtype(torch::kFloat32).device(cuda_device(device_id)));
}

std::string resolve_path(const std::string& path) {
    return std::filesystem::absolute(std::filesystem::path(path)).string();
}

mmltk::DatasetLoader::Config make_loader_config(const std::string& compiled_path, size_t batch_size, bool shuffle,
                                                int prefetch_factor, int gather_workers,
                                                const std::string& cpu_affinity, int device_id, uint64_t seed,
                                                uint32_t batch_shard_rank, uint32_t batch_shard_count) {
    mmltk::DatasetLoader::Config config;
    config.compiled_path = resolve_path(compiled_path);
    config.batch_size = batch_size;
    config.shuffle = shuffle;
    config.prefetch_factor = prefetch_factor;
    config.gather_workers = std::clamp(gather_workers, 1, prefetch_factor);
    config.cpu_affinity = cpu_affinity;
    config.device_id = device_id;
    config.seed = seed;
    config.batch_shard_rank = batch_shard_rank;
    config.batch_shard_count = batch_shard_count;
    config.drop_last = true;
    return config;
}

PreparedTargets build_targets(const mmltk::Batch& batch, int image_height, int image_width, bool include_masks,
                              bool require_masks, int device_id, TargetScratch& scratch,
                              AugmentationBatchPlan* augmentation_plan) {
    MMLTK_PROFILE_SCOPE("rfdetr.targets.total");
    if (augmentation_plan != nullptr && augmentation_plan->active_size != batch.num_images) {
        throw std::runtime_error("augmentation plan batch size does not match target batch");
    }
    PreparedTargets prepared;
    prepared.targets.reserve(batch.num_images);
    scratch.ensure_batch(batch.num_images, image_height, image_width, device_id);
    MMLTK_PROFILE_ADD("rfdetr.targets.images", batch.num_images);
    const auto device = cuda_device(device_id);
    const auto gpu_float = torch::TensorOptions().dtype(torch::kFloat32).device(device);
    const auto gpu_int64 = torch::TensorOptions().dtype(torch::kInt64).device(device);
    const float inv_w = 1.0f / static_cast<float>(image_width);
    const float inv_h = 1.0f / static_cast<float>(image_height);
    auto& offsets = scratch.offsets;
    auto& counts = scratch.counts;
    int64_t maximum_instances = 0;
    for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
        const auto& entry = batch.label_index[batch.image_indices[image_pos]];
        maximum_instances += static_cast<int64_t>(entry.num_instances);
    }
    if (augmentation_plan != nullptr && augmentation_plan->copy_paste_enabled) {
        maximum_instances += static_cast<int64_t>(batch.num_images);
    }
    scratch.ensure_instance_capacity(std::max<int64_t>(maximum_instances, 1));
    if (include_masks) {
        scratch.ensure_packed_mask_capacity(std::max<int64_t>(maximum_instances, 1), image_height, image_width);
    }

    auto image_ids_cpu = scratch.image_ids_cpu.narrow(0, 0, static_cast<int64_t>(batch.num_images));
    auto offsets_cpu = scratch.offsets_cpu.narrow(0, 0, static_cast<int64_t>(batch.num_images));
    auto counts_cpu = scratch.counts_cpu.narrow(0, 0, static_cast<int64_t>(batch.num_images));
    auto image_ids = image_ids_cpu.accessor<int64_t, 1>();
    auto offset_values = offsets_cpu.accessor<int64_t, 1>();
    auto count_values = counts_cpu.accessor<int64_t, 1>();
    torch::Tensor packed_masks_cpu;
    int64_t* packed_masks_data = nullptr;
    if (include_masks && maximum_instances > 0) {
        packed_masks_cpu = scratch.packed_masks_cpu.narrow(0, 0, maximum_instances);
        std::memset(packed_masks_cpu.data_ptr<int64_t>(), 0,
                    static_cast<size_t>(maximum_instances) * static_cast<size_t>(scratch.mask_words_per_instance) *
                        sizeof(int64_t));
        packed_masks_data = packed_masks_cpu.data_ptr<int64_t>();
    }

    int64_t total_instances = 0;
    int64_t mask_instances = 0;
    int64_t dropped_instances = 0;
    if (maximum_instances > 0) {
        std::memset(scratch.iscrowd_cpu.data_ptr<int64_t>(), 0,
                    static_cast<size_t>(maximum_instances) * sizeof(int64_t));
        std::fill_n(scratch.occluder_mask_indices_cpu.data_ptr<int64_t>(), maximum_instances, int64_t{-1});
    }
    auto boxes = scratch.boxes_cpu.accessor<float, 2>();
    auto labels = scratch.labels_cpu.accessor<int64_t, 1>();
    auto areas = scratch.area_cpu.accessor<float, 1>();
    auto inverse_transforms = scratch.inverse_transforms_cpu.accessor<float, 2>();
    auto occluder_indices = scratch.occluder_mask_indices_cpu.accessor<int64_t, 1>();
    auto occluder_transforms = scratch.occluder_inverse_transforms_cpu.accessor<float, 2>();

    const std::array<float, 6> identity{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    {
        MMLTK_PROFILE_SCOPE("rfdetr.targets.pack_instances");
        for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
            const uint32_t dataset_index = batch.image_indices[image_pos];
            const auto& entry = batch.label_index[dataset_index];
            auto* image_plan = augmentation_plan != nullptr ? &augmentation_plan->images[image_pos] : nullptr;
            if (image_plan != nullptr) {
                image_plan->cache_source_target_index = -1;
                image_plan->cache_source_label = -1;
                image_plan->cache_source_area = 0.0F;
                image_plan->paste_target_index = -1;
            }
            const auto& forward = image_plan != nullptr ? image_plan->forward : identity;
            const auto& inverse = image_plan != nullptr ? image_plan->inverse : identity;
            auto* paste_plan = image_plan;
            if (paste_plan != nullptr && paste_plan->paste_donor_slot < 0) {
                paste_plan = nullptr;
            }
            const int64_t image_offset = total_instances;
            int64_t retained_candidates = 0;
            image_ids[static_cast<int64_t>(image_pos)] = static_cast<int64_t>(dataset_index) + 1;

            for (int64_t instance_index = 0; instance_index < static_cast<int64_t>(entry.num_instances);
                 ++instance_index) {
                const auto& instance =
                    batch.labels[static_cast<size_t>(entry.label_begin) + static_cast<size_t>(instance_index)];
                const std::array<float, 4> source_box{
                    static_cast<float>(instance.bbox_x1) * inv_w,
                    static_cast<float>(instance.bbox_y1) * inv_h,
                    static_cast<float>(instance.bbox_x2) * inv_w,
                    static_cast<float>(instance.bbox_y2) * inv_h,
                };
                const std::array<float, 4> transformed = transform_box_xyxy(source_box, forward);
                const float transformed_area = box_area(transformed);
                const float occluded_area =
                    paste_plan != nullptr ? box_intersection_area(transformed, paste_plan->paste_output_box) : 0.0F;
                if (transformed_area <= 0.0F || occluded_area >= transformed_area) {
                    ++dropped_instances;
                    continue;
                }

                const int64_t target_index = total_instances++;
                boxes[target_index][0] = (transformed[0] + transformed[2]) * 0.5F;
                boxes[target_index][1] = (transformed[1] + transformed[3]) * 0.5F;
                boxes[target_index][2] = transformed[2] - transformed[0];
                boxes[target_index][3] = transformed[3] - transformed[1];
                labels[target_index] = static_cast<int64_t>(instance.class_id);
                std::copy(inverse.begin(), inverse.end(), &inverse_transforms[target_index][0]);
                std::copy(paste_plan != nullptr ? paste_plan->paste_inverse.begin() : identity.begin(),
                          paste_plan != nullptr ? paste_plan->paste_inverse.end() : identity.end(),
                          &occluder_transforms[target_index][0]);

                float source_area = std::max(0.0F, source_box[2] - source_box[0]) * static_cast<float>(image_width) *
                                    std::max(0.0F, source_box[3] - source_box[1]) * static_cast<float>(image_height);
                if (include_masks) {
                    ++mask_instances;
                    const uint16_t pair_count = instance.mask_rle_pairs;
                    const size_t pair_offset = static_cast<size_t>(instance.mask_rle_offset) / sizeof(mmltk::RLEPair);
                    if (pair_count == 0 && require_masks) {
                        throw std::runtime_error("segmentation training requires decodable masks for every instance");
                    }
                    source_area = 0.0F;
                    for (size_t pair_index = 0; pair_index < pair_count; ++pair_index) {
                        const auto& pair = batch.rle_pairs[pair_offset + pair_index];
                        const size_t length = pair.length;
                        if (static_cast<size_t>(pair.start) + length >
                            static_cast<size_t>(image_height) * static_cast<size_t>(image_width)) {
                            throw std::runtime_error("mask_rle run exceeds compiled image bounds");
                        }
                        int64_t* mask_words =
                            packed_masks_data +
                            static_cast<size_t>(target_index) * static_cast<size_t>(scratch.mask_words_per_instance);
                        set_packed_mask_range(mask_words, static_cast<size_t>(pair.start), length);
                        source_area += static_cast<float>(length);
                    }
                }
                const float visibility = std::max(0.0F, 1.0F - occluded_area / transformed_area);
                areas[target_index] =
                    source_area * (image_plan != nullptr ? image_plan->area_scale : 1.0F) * visibility;

                ++retained_candidates;
                if (image_plan != nullptr &&
                    reservoir_select(image_plan->cache_choice, retained_candidates, instance_index)) {
                    image_plan->cache_source_target_index = target_index;
                    image_plan->cache_source_label = static_cast<int64_t>(instance.class_id);
                    image_plan->cache_source_dataset_index = dataset_index;
                    image_plan->cache_source_area = source_area;
                    image_plan->cache_source_box = source_box;
                }
            }

            const int64_t original_end = total_instances;
            if (paste_plan != nullptr) {
                const int64_t paste_index = total_instances++;
                const auto& paste_box = paste_plan->paste_output_box;
                boxes[paste_index][0] = (paste_box[0] + paste_box[2]) * 0.5F;
                boxes[paste_index][1] = (paste_box[1] + paste_box[3]) * 0.5F;
                boxes[paste_index][2] = paste_box[2] - paste_box[0];
                boxes[paste_index][3] = paste_box[3] - paste_box[1];
                labels[paste_index] = paste_plan->paste_label;
                const float inverse_scale = paste_plan->paste_inverse[0];
                const float scale = inverse_scale != 0.0F ? 1.0F / inverse_scale : 1.0F;
                const float source_box_area = std::max(box_area(paste_plan->paste_source_box), 1.0e-12F);
                const float clip_fraction = std::min(1.0F, box_area(paste_box) / (source_box_area * scale * scale));
                areas[paste_index] = paste_plan->paste_source_area * scale * scale * clip_fraction;
                std::copy(paste_plan->paste_inverse.begin(), paste_plan->paste_inverse.end(),
                          &inverse_transforms[paste_index][0]);
                std::copy(identity.begin(), identity.end(), &occluder_transforms[paste_index][0]);
                occluder_indices[paste_index] = -1;
                paste_plan->paste_target_index = paste_index;
                for (int64_t target_index = image_offset; target_index < original_end; ++target_index) {
                    occluder_indices[target_index] = paste_index;
                }
                if (include_masks) {
                    ++mask_instances;
                }
            }

            offsets[image_pos] = image_offset;
            counts[image_pos] = total_instances - image_offset;
            offset_values[static_cast<int64_t>(image_pos)] = image_offset;
            count_values[static_cast<int64_t>(image_pos)] = counts[image_pos];
        }
    }
    MMLTK_PROFILE_ADD("rfdetr.targets.instances", total_instances);
    MMLTK_PROFILE_ADD("rfdetr.targets.mask_instances", mask_instances);
    MMLTK_PROFILE_ADD("rfdetr.targets.augmentation_dropped", dropped_instances);

    scratch.ensure_copy_resources(device_id);
    torch::Tensor boxes_gpu = torch::zeros({0, 4}, gpu_float);
    torch::Tensor labels_gpu = torch::zeros({0}, gpu_int64);
    torch::Tensor area_gpu = torch::zeros({0}, gpu_float);
    torch::Tensor iscrowd_gpu = torch::zeros({0}, gpu_int64);
    std::optional<PackedTargetMasks> packed_masks_gpu;
    torch::Tensor image_ids_gpu;
    {
        MMLTK_PROFILE_SCOPE("rfdetr.targets.h2d");
        c10::cuda::CUDAGuard device_guard(cuda_device_index(device_id));
        const auto copy_stream = require_copy_stream(scratch);
        c10::cuda::CUDAStreamGuard stream_guard(copy_stream);
        image_ids_gpu = image_ids_cpu.to(device, torch::kInt64, true, false);
        scratch.offsets_gpu.narrow(0, 0, static_cast<int64_t>(batch.num_images)).copy_(offsets_cpu, true);
        scratch.counts_gpu.narrow(0, 0, static_cast<int64_t>(batch.num_images)).copy_(counts_cpu, true);
        if (total_instances > 0) {
            boxes_gpu = scratch.boxes_cpu.narrow(0, 0, total_instances).to(device, torch::kFloat32, true, false);
            labels_gpu = scratch.labels_cpu.narrow(0, 0, total_instances).to(device, torch::kInt64, true, false);
            area_gpu = scratch.area_cpu.narrow(0, 0, total_instances).to(device, torch::kFloat32, true, false);
            iscrowd_gpu = scratch.iscrowd_cpu.narrow(0, 0, total_instances).to(device, torch::kInt64, true, false);
            if (include_masks) {
                const bool transformed = augmentation_plan != nullptr && (augmentation_plan->transforms_geometry ||
                                                                          augmentation_plan->copy_paste_enabled);
                packed_masks_gpu = PackedTargetMasks{
                    scratch.packed_masks_cpu.narrow(0, 0, total_instances).to(device, torch::kInt64, true, false),
                    image_height,
                    image_width,
                    transformed ? scratch.inverse_transforms_cpu.narrow(0, 0, total_instances)
                                      .to(device, torch::kFloat32, true, false)
                                : torch::Tensor{},
                    transformed ? scratch.occluder_mask_indices_cpu.narrow(0, 0, total_instances)
                                      .to(device, torch::kInt64, true, false)
                                : torch::Tensor{},
                    transformed ? scratch.occluder_inverse_transforms_cpu.narrow(0, 0, total_instances)
                                      .to(device, torch::kFloat32, true, false)
                                : torch::Tensor{},
                };
            }
        } else if (include_masks) {
            packed_masks_gpu = PackedTargetMasks{
                torch::zeros({0, packed_mask_words_for_shape(image_height, image_width)}, gpu_int64),
                image_height,
                image_width,
                {},
                {},
                {},
            };
        }
    }
    {
        c10::cuda::CUDAGuard device_guard(cuda_device_index(device_id));
        const auto copy_stream = require_copy_stream(scratch);
        c10::cuda::CUDAStreamGuard stream_guard(copy_stream);
        check_cuda_status(cudaEventRecord(scratch.copy_complete_event, copy_stream.stream()),
                          "cudaEventRecord for target copy");
        scratch.copy_pending = true;
    }

    prepared.orig_sizes = scratch.batch.sizes_view(static_cast<int64_t>(batch.num_images));
    prepared.nested_mask = scratch.batch.nested_mask_view(static_cast<int64_t>(batch.num_images));
    prepared.all_boxes = boxes_gpu;
    prepared.all_labels = labels_gpu;
    prepared.all_area = area_gpu;
    prepared.all_iscrowd = iscrowd_gpu;
    prepared.target_offsets = scratch.offsets_gpu.narrow(0, 0, static_cast<int64_t>(batch.num_images));
    prepared.target_counts = scratch.counts_gpu.narrow(0, 0, static_cast<int64_t>(batch.num_images));
    prepared.target_indices =
        total_instances > 0 ? scratch.target_indices_gpu.narrow(0, 0, total_instances) : torch::empty({0}, gpu_int64);
    prepared.packed_masks = std::move(packed_masks_gpu);
    prepared.offsets = scratch.offsets;
    prepared.counts = scratch.counts;
    {
        MMLTK_PROFILE_SCOPE("rfdetr.targets.pack_views");
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

}  
