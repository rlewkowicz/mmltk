#include "cuda_test_utils.h"
#include "dataset_compiler.h"
#include "dataset_loader.h"
#include "fastloader/rfdetr/detection_ops.h"
#include "fastloader/rfdetr/target_builder.h"
#include "test_fixture.h"

#include <ATen/TensorAccessor.h>
#include <ATen/TensorIndexing.h>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace {

using namespace torch::indexing;

torch::Tensor unpack_packed_masks(const fastloader::rfdetr::PackedTargetMasks& masks) {
    const auto bits = masks.bits.to(torch::kCPU, torch::kInt64).contiguous();
    auto dense = torch::zeros(
        {bits.size(0), masks.height, masks.width},
        torch::TensorOptions().dtype(torch::kFloat32));
    const auto* bits_ptr = reinterpret_cast<const uint64_t*>(bits.data_ptr<int64_t>());
    auto dense_a = dense.accessor<float, 3>();
    const int64_t words_per_mask = bits.size(1);
    for (int64_t mask_index = 0; mask_index < bits.size(0); ++mask_index) {
        const auto* mask_words = bits_ptr + mask_index * words_per_mask;
        for (int64_t y = 0; y < masks.height; ++y) {
            for (int64_t x = 0; x < masks.width; ++x) {
                const int64_t pixel_index = y * masks.width + x;
                if (((mask_words[pixel_index >> 6] >> (pixel_index & 63)) & uint64_t{1}) != 0) {
                    dense_a[mask_index][y][x] = 1.0f;
                }
            }
        }
    }
    return dense;
}

void test_target_builder_with_masks(int first_class_id) {
    constexpr int kDeviceId = 0;
    CUDA_ASSERT_OK(cudaSetDevice(kDeviceId));

    const fastloader::testsupport::FixtureSpec fixture{
        "/tmp/fastloader_rfdetr_target_builder",
        "train",
        65,
        65,
        12,
        first_class_id,
    };
    fastloader::testsupport::create_synthetic_dataset(fixture);

    fastloader::CompilerConfig compile_config;
    compile_config.source_dir = fastloader::testsupport::dataset_dir(fixture);
    compile_config.output_dir = fastloader::testsupport::compiled_dir(fixture);
    compile_config.split = fixture.split;
    compile_config.target_width = fixture.width;
    compile_config.target_height = fixture.height;
    compile_config.num_workers = 2;
    fastloader::DatasetCompiler::compile(compile_config);

    fastloader::DatasetLoader loader(fastloader::rfdetr::make_loader_config(
        fastloader::testsupport::compiled_bin_path(fixture),
        static_cast<size_t>(fixture.num_images),
        false,
        1,
        0,
        "",
        kDeviceId,
        42));
    std::vector<uint32_t> image_indices(static_cast<size_t>(fixture.num_images));
    std::iota(image_indices.begin(), image_indices.end(), 0u);
    fastloader::Batch batch{};
    batch.num_images = static_cast<size_t>(fixture.num_images);
    batch.host_images = nullptr;
    batch.device_images = nullptr;
    batch.label_index = loader.label_index();
    batch.labels = loader.label_data();
    batch.rle_pairs = loader.rle_data();
    batch.image_indices = image_indices.data();
    batch.slot_index = 0;
    batch.lease_id = 0;

    const auto device_index = fastloader::rfdetr::cuda_device_index(kDeviceId);
    c10::cuda::CUDAGuard device_guard(device_index);

    fastloader::rfdetr::TargetScratch scratch;
    auto prepared = fastloader::rfdetr::build_targets(
        batch,
        fixture.height,
        fixture.width,
        true,
        true,
        kDeviceId,
        scratch);
    scratch.wait_for_pending_copy();
    CUDA_ASSERT_OK(cudaDeviceSynchronize());

    assert(prepared.targets.size() == static_cast<size_t>(fixture.num_images));
    assert(prepared.offsets.size() == prepared.targets.size());
    assert(prepared.counts.size() == prepared.targets.size());
    assert(prepared.all_boxes.size(0) == 2);
    assert(prepared.all_labels.size(0) == 2);
    assert(prepared.all_area.size(0) == 2);
    assert(prepared.packed_masks.has_value());
    assert(prepared.packed_masks->bits.defined());
    assert(prepared.packed_masks->bits.size(0) == 2);
    assert(prepared.packed_masks->height == fixture.height);
    assert(prepared.packed_masks->width == fixture.width);
    assert(prepared.packed_masks->bits.size(1) ==
           (static_cast<int64_t>(fixture.height) * static_cast<int64_t>(fixture.width) + 63) / 64);
    assert(prepared.orig_sizes.size(0) == fixture.num_images);
    assert(prepared.orig_sizes.size(1) == 2);
    assert(prepared.nested_mask.size(0) == fixture.num_images);
    assert(prepared.nested_mask.size(1) == fixture.height);
    assert(prepared.nested_mask.size(2) == fixture.width);
    assert(!prepared.nested_mask.any().item<bool>());

    for (size_t index = 0; index < 10; ++index) {
        assert(prepared.counts[index] == 0);
        assert(prepared.targets[index].boxes.numel() == 0);
    }
    for (size_t index = 10; index < prepared.targets.size(); ++index) {
        assert(prepared.counts[index] == 1);
        assert(prepared.targets[index].boxes.size(0) == 1);
    }

    const auto boxes_cpu = prepared.all_boxes.cpu();
    const auto labels_cpu = prepared.all_labels.cpu();
    const auto area_cpu = prepared.all_area.cpu();
    const auto boxes = boxes_cpu.accessor<float, 2>();
    const auto labels = labels_cpu.accessor<int64_t, 1>();

    const auto expected_center = 20.0f / 65.0f;
    const auto expected_extent = 20.0f / 65.0f;
    assert(std::fabs(boxes[0][0] - expected_center) < 1e-6f);
    assert(std::fabs(boxes[0][1] - expected_center) < 1e-6f);
    assert(std::fabs(boxes[0][2] - expected_extent) < 1e-6f);
    assert(std::fabs(boxes[0][3] - expected_extent) < 1e-6f);
    assert(labels[0] == 0);
    assert(labels[1] == 1);
    assert(std::fabs(area_cpu.sum().item<float>() - 800.0f) < 1e-6f);

    const auto dense_masks = unpack_packed_masks(*prepared.packed_masks);
    assert(dense_masks.is_contiguous());
    assert(dense_masks.select(0, 0).sum().item<int64_t>() == 400);
    assert(dense_masks.select(0, 1).sum().item<int64_t>() == 400);
    assert(dense_masks.index({0, 15, 15}).item<float>() == 1.0f);
    assert(dense_masks.index({0, 40, 40}).item<float>() == 0.0f);
    assert(dense_masks.index({1, 15, 15}).item<float>() == 1.0f);
    assert(dense_masks.index({1, 40, 40}).item<float>() == 0.0f);
}

} // namespace

int main() {
    test_target_builder_with_masks(/*first_class_id=*/1);
    test_target_builder_with_masks(/*first_class_id=*/0);
    return 0;
}
