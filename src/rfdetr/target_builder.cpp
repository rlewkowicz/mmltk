#include "mmltk/rfdetr/target_builder.h"

#include "compiled_format.h"
#include "mmltk_logging.h"
#include "profile_utils.h"
#include "rfdetr/torch_cuda_utils.h"

#include <filesystem>
#include <algorithm>
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
        const uint64_t mask = fill_bits == static_cast<size_t>(kMaskWordBits)
            ? full_mask
            : ((uint64_t{1} << fill_bits) - 1);
        words[word_index] |= (mask << bit_index);
        bit_offset += fill_bits;
        remaining -= fill_bits;
    }
}

} // namespace

c10::DeviceIndex cuda_device_index(int device_id) {
    return checked_device_index(device_id);
}

torch::Device cuda_device(int device_id) {
    return {torch::kCUDA, cuda_device_index(device_id)};
}

void BatchStaticTensors::ensure(int64_t batch_size, int height, int width, int target_device_id) {
    MMLTK_PROFILE_SCOPE("rfdetr.targets.static_tensors");
    const bool stale_shape =
        device_id != target_device_id || image_height != height || image_width != width;
    if (!stale_shape && batch_capacity >= batch_size && sizes.defined() && nested_mask.defined()) {
        return;
    }

    device_id = target_device_id;
    image_height = height;
    image_width = width;
    batch_capacity = std::max(batch_capacity, batch_size);
    const auto device = cuda_device(device_id);
    sizes = make_size_tensor(batch_capacity,
                             static_cast<int64_t>(image_height),
                             static_cast<int64_t>(image_width),
                             device);
    nested_mask = torch::zeros(
        {batch_capacity, image_height, image_width},
        torch::TensorOptions().dtype(torch::kBool).device(device)
    );
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
        mmltk::logging::logger("rfdetr.target_builder")->error(
            "fatal: failed to finish target copy before destruction: {}",
            ex.what());
        std::terminate();
    } catch (...) {
        mmltk::logging::logger("rfdetr.target_builder")->error(
            "fatal: failed to finish target copy before destruction with unknown exception");
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
    if (!image_ids_cpu.defined() || image_ids_cpu.size(0) < static_cast<int64_t>(batch_size)) {
        image_ids_cpu = torch::empty(
            {std::max<int64_t>(batch.batch_capacity, static_cast<int64_t>(batch_size))},
            torch::TensorOptions().dtype(torch::kInt64).device(torch::kCPU).pinned_memory(true)
        );
    }
}

void TargetScratch::ensure_instance_capacity(int64_t instances) {
    MMLTK_PROFILE_SCOPE("rfdetr.targets.ensure_instances");
    const bool tensors_ready =
        boxes_cpu.defined() &&
        labels_cpu.defined() &&
        area_cpu.defined() &&
        iscrowd_cpu.defined() &&
        boxes_cpu.size(1) == 4;
    if (instance_capacity >= instances && tensors_ready) {
        return;
    }

    instance_capacity = std::max(instance_capacity, instances);
    const auto cpu_float =
        torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU).pinned_memory(true);
    const auto cpu_int64 =
        torch::TensorOptions().dtype(torch::kInt64).device(torch::kCPU).pinned_memory(true);

    boxes_cpu = torch::empty({instance_capacity, 4}, cpu_float);
    labels_cpu = torch::empty({instance_capacity}, cpu_int64);
    area_cpu = torch::empty({instance_capacity}, cpu_float);
    iscrowd_cpu = torch::empty({instance_capacity}, cpu_int64);
}

void TargetScratch::ensure_packed_mask_capacity(int64_t instances, int height, int width) {
    const bool shape_matches = mask_height == height && mask_width == width;
    if (packed_masks_cpu.defined() &&
        shape_matches &&
        packed_masks_cpu.size(0) >= instances) {
        return;
    }

    mask_height = height;
    mask_width = width;
    mask_words_per_instance = packed_mask_words_for_shape(mask_height, mask_width);
    const int64_t mask_capacity = std::max<int64_t>(instances, 1);
    packed_masks_cpu = torch::zeros(
        {mask_capacity, mask_words_per_instance},
        torch::TensorOptions().dtype(torch::kInt64).device(torch::kCPU).pinned_memory(true)
    );
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

LoaderBatchGuard::LoaderBatchGuard(mmltk::DatasetLoader& loader,
                                   const mmltk::Batch& batch,
                                   int device_id)
    : loader_(&loader), batch_(batch), device_id_(device_id) {
    const auto device_index = cuda_device_index(device_id_);
    c10::cuda::CUDAGuard device_guard(device_index);
    consumer_stream_ = reinterpret_cast<void*>(c10::cuda::getCurrentCUDAStream(device_index).stream());
    loader_->handoff_batch(batch_, consumer_stream_);
}

LoaderBatchGuard::~LoaderBatchGuard() {
    try {
        c10::cuda::CUDAGuard device_guard(cuda_device_index(device_id_));
        loader_->release_batch(batch_, consumer_stream_);
    } catch (const std::exception& ex) {
        mmltk::logging::logger("rfdetr.target_builder")->error(
            "fatal: failed to release dataset batch: {}",
            ex.what());
        std::terminate();
    } catch (...) {
        mmltk::logging::logger("rfdetr.target_builder")->error(
            "fatal: failed to release dataset batch with unknown exception");
        std::terminate();
    }
}

torch::Tensor make_size_tensor(int64_t batch_size,
                               int64_t image_height,
                               int64_t image_width,
                               const torch::Device& device) {
    auto sizes = torch::empty(
        {batch_size, 2},
        torch::TensorOptions().dtype(torch::kInt64).device(device)
    );
    sizes.select(1, 0).fill_(image_height);
    sizes.select(1, 1).fill_(image_width);
    return sizes;
}

torch::Tensor make_device_batch_tensor(const mmltk::Batch& batch,
                                       int device_id,
                                       int64_t image_height,
                                       int64_t image_width) {
    MMLTK_PROFILE_SCOPE("rfdetr.pybind.device_batch_tensor");
    return torch::from_blob(
        const_cast<float*>(batch.device_images),
        {static_cast<int64_t>(batch.num_images), 3, image_height, image_width},
        torch::TensorOptions().dtype(torch::kFloat32).device(cuda_device(device_id))
    );
}

std::string resolve_path(const std::string& path) {
    return std::filesystem::absolute(std::filesystem::path(path)).string();
}

mmltk::DatasetLoader::Config make_loader_config(const std::string& compiled_path,
                                                     size_t batch_size,
                                                     bool shuffle,
                                                     int prefetch_factor,
                                                     int gather_workers,
                                                     const std::string& cpu_affinity,
                                                     int device_id,
                                                     uint64_t seed,
                                                     uint32_t batch_shard_rank,
                                                     uint32_t batch_shard_count) {
    mmltk::DatasetLoader::Config config;
    config.compiled_path = resolve_path(compiled_path);
    config.batch_size = batch_size;
    config.shuffle = shuffle;
    config.prefetch_factor = prefetch_factor;
    config.gather_workers = shuffle ? std::min(gather_workers, prefetch_factor) : 0;
    config.cpu_affinity = cpu_affinity;
    config.device_id = device_id;
    config.seed = seed;
    config.batch_shard_rank = batch_shard_rank;
    config.batch_shard_count = batch_shard_count;
    config.drop_last = true;
    return config;
}

PreparedTargets build_targets(const mmltk::Batch& batch,
                              int image_height,
                              int image_width,
                              bool include_masks,
                              bool require_masks,
                              int device_id,
                              TargetScratch& scratch) {
    MMLTK_PROFILE_SCOPE("rfdetr.targets.total");
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
    int64_t total_instances = 0;
    auto image_ids_cpu = scratch.image_ids_cpu.narrow(0, 0, static_cast<int64_t>(batch.num_images));
    auto image_ids = image_ids_cpu.accessor<int64_t, 1>();
    {
        MMLTK_PROFILE_SCOPE("rfdetr.targets.index_targets");
        for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
            const uint32_t dataset_index = batch.image_indices[image_pos];
            const auto& entry = batch.label_index[dataset_index];
            offsets[image_pos] = total_instances;
            counts[image_pos] = static_cast<int64_t>(entry.num_instances);
            total_instances += counts[image_pos];
            image_ids[static_cast<int64_t>(image_pos)] = static_cast<int64_t>(dataset_index) + 1;
        }
    }
    MMLTK_PROFILE_ADD("rfdetr.targets.instances", total_instances);

    scratch.ensure_copy_resources(device_id);
    torch::Tensor boxes_gpu = torch::zeros({0, 4}, gpu_float);
    torch::Tensor labels_gpu = torch::zeros({0}, gpu_int64);
    torch::Tensor area_gpu = torch::zeros({0}, gpu_float);
    torch::Tensor iscrowd_gpu = torch::zeros({0}, gpu_int64);
    std::optional<PackedTargetMasks> packed_masks_gpu;
    torch::Tensor image_ids_gpu;
    {
        c10::cuda::CUDAGuard device_guard(cuda_device_index(device_id));
        const auto copy_stream = require_copy_stream(scratch);
        c10::cuda::CUDAStreamGuard stream_guard(copy_stream);
        image_ids_gpu = image_ids_cpu.to(device, torch::kInt64, true, false);
    }

    if (total_instances > 0) {
        scratch.ensure_instance_capacity(total_instances);
        if (include_masks) {
            scratch.ensure_packed_mask_capacity(total_instances, image_height, image_width);
        }
        auto boxes_cpu = scratch.boxes_cpu.narrow(0, 0, total_instances);
        auto labels_cpu = scratch.labels_cpu.narrow(0, 0, total_instances);
        auto area_cpu = scratch.area_cpu.narrow(0, 0, total_instances);
        auto iscrowd_cpu = scratch.iscrowd_cpu.narrow(0, 0, total_instances);
        torch::Tensor packed_masks_cpu;
        int64_t* packed_masks_data = nullptr;
        if (include_masks) {
            packed_masks_cpu = scratch.packed_masks_cpu.narrow(0, 0, total_instances);
            std::memset(
                packed_masks_cpu.data_ptr<int64_t>(),
                0,
                static_cast<size_t>(total_instances) * static_cast<size_t>(scratch.mask_words_per_instance) * sizeof(int64_t));
            packed_masks_data = packed_masks_cpu.data_ptr<int64_t>();
        }
        std::memset(iscrowd_cpu.data_ptr<int64_t>(),
                    0,
                    static_cast<size_t>(total_instances) * sizeof(int64_t));
        auto boxes = boxes_cpu.accessor<float, 2>();
        auto labels = labels_cpu.accessor<int64_t, 1>();
        auto areas = area_cpu.accessor<float, 1>();

        int64_t mask_instances = 0;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.targets.pack_instances");
            for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
                const uint32_t dataset_index = batch.image_indices[image_pos];
                const auto& entry = batch.label_index[dataset_index];
                const auto label_begin = static_cast<size_t>(entry.label_begin);
                const int64_t batch_offset = offsets[image_pos];
                const int64_t count = counts[image_pos];
                for (int64_t instance_index = 0; instance_index < count; ++instance_index) {
                    const int64_t target_index = batch_offset + instance_index;
                    const auto& instance = batch.labels[label_begin + static_cast<size_t>(instance_index)];
                    const auto x1 = static_cast<float>(instance.bbox_x1);
                    const auto y1 = static_cast<float>(instance.bbox_y1);
                    const auto x2 = static_cast<float>(instance.bbox_x2);
                    const auto y2 = static_cast<float>(instance.bbox_y2);
                    const float width = std::max(0.0f, x2 - x1);
                    const float height = std::max(0.0f, y2 - y1);

                    boxes[target_index][0] = (x1 + x2) * 0.5f * inv_w;
                    boxes[target_index][1] = (y1 + y2) * 0.5f * inv_h;
                    boxes[target_index][2] = width * inv_w;
                    boxes[target_index][3] = height * inv_h;
                    // Compiled labels are already normalized into RF-DETR's
                    // 0-based contiguous training space.
                    labels[target_index] = static_cast<int64_t>(instance.class_id);

                    if (!include_masks) {
                        areas[target_index] = width * height;
                        continue;
                    }

                    ++mask_instances;
                    const uint16_t pair_count = instance.mask_rle_pairs;
                    const size_t pair_offset =
                        static_cast<size_t>(instance.mask_rle_offset) / sizeof(mmltk::RLEPair);
                    if (pair_count == 0) {
                        if (require_masks) {
                            throw std::runtime_error(
                                "segmentation training requires decodable masks for every instance");
                        }
                        areas[target_index] = 0.0f;
                        continue;
                    }

                    float area = 0.0f;
                    for (size_t pair_index = 0; pair_index < pair_count; ++pair_index) {
                        const auto& pair = batch.rle_pairs[pair_offset + pair_index];
                        const size_t length = pair.length;
                        if (static_cast<size_t>(pair.start) + length >
                            static_cast<size_t>(image_height) * static_cast<size_t>(image_width)) {
                            throw std::runtime_error("mask_rle run exceeds compiled image bounds");
                        }
                        if (packed_masks_data != nullptr) {
                            int64_t* mask_words =
                                packed_masks_data +
                                static_cast<size_t>(target_index) * static_cast<size_t>(scratch.mask_words_per_instance);
                            set_packed_mask_range(mask_words, static_cast<size_t>(pair.start), length);
                        }
                        area += static_cast<float>(length);
                    }
                    areas[target_index] = area;
                }
            }
        }
        MMLTK_PROFILE_ADD("rfdetr.targets.mask_instances", mask_instances);

        {
            MMLTK_PROFILE_SCOPE("rfdetr.targets.h2d");
            c10::cuda::CUDAGuard device_guard(cuda_device_index(device_id));
            const auto copy_stream = require_copy_stream(scratch);
            c10::cuda::CUDAStreamGuard stream_guard(copy_stream);
            boxes_gpu = boxes_cpu.to(device, torch::kFloat32, true, false);
            labels_gpu = labels_cpu.to(device, torch::kInt64, true, false);
            area_gpu = area_cpu.to(device, torch::kFloat32, true, false);
            iscrowd_gpu = iscrowd_cpu.to(device, torch::kInt64, true, false);
            if (include_masks) {
                packed_masks_gpu = PackedTargetMasks{
                    packed_masks_cpu.to(device, torch::kInt64, true, false),
                    image_height,
                    image_width,
                };
            }
        }
    } else if (include_masks) {
        packed_masks_gpu = PackedTargetMasks{
            torch::zeros({0, packed_mask_words_for_shape(image_height, image_width)}, gpu_int64),
            image_height,
            image_width,
        };
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

} // namespace mmltk::rfdetr
