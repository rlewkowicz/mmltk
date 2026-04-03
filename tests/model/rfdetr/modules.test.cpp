#include "mmltk/rfdetr/modules.h"
#include "rfdetr/ms_deform_attn_op.h"

#include <ATen/TensorIndexing.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include "support/catch2_compat.hpp"
#include <cmath>
#include <torch/nn/functional.h>

namespace F = torch::nn::functional;
using namespace torch::indexing;

namespace {

torch::Tensor ms_deform_attn_reference(const torch::Tensor& value,
                                       const torch::Tensor& spatial_shapes,
                                       const torch::Tensor& sampling_locations,
                                       const torch::Tensor& attention_weights) {
    const int64_t batch = value.size(0);
    const int64_t num_heads = value.size(2);
    const int64_t head_dim = value.size(3);
    const int64_t len_q = sampling_locations.size(1);
    const int64_t num_levels = spatial_shapes.size(0);
    const int64_t num_points = sampling_locations.size(4);

    auto sampling_grids = 2.0 * sampling_locations - 1.0;
    std::vector<torch::Tensor> sampled_values;
    sampled_values.reserve(static_cast<size_t>(num_levels));

    int64_t start = 0;
    for (int64_t level = 0; level < num_levels; ++level) {
        const auto height = spatial_shapes.index({level, 0}).item<int64_t>();
        const auto width = spatial_shapes.index({level, 1}).item<int64_t>();
        auto value_level = value.narrow(1, start, height * width)
                               .flatten(2)
                               .transpose(1, 2)
                               .reshape({batch * num_heads, head_dim, height, width});
        auto sampling_grid_level =
            sampling_grids.index({Slice(), Slice(), Slice(), level}).transpose(1, 2).flatten(0, 1);
        sampled_values.push_back(F::grid_sample(
            value_level,
            sampling_grid_level,
            F::GridSampleFuncOptions().mode(torch::kBilinear).padding_mode(torch::kZeros).align_corners(false)));
        start += height * width;
    }

    auto sampled = torch::stack(sampled_values, -2).flatten(-2);
    auto normalized_attention =
        attention_weights.transpose(1, 2).reshape({batch * num_heads, 1, len_q, num_levels * num_points});
    auto output = (sampled * normalized_attention).sum(-1).view({batch, num_heads * head_dim, len_q});
    return output.transpose(1, 2).contiguous();
}

void test_detection_head_shapes() {
    mmltk::rfdetr::DetectionHead head(/*hidden_dim=*/256, /*num_classes=*/91);
    const auto inputs = torch::randn({2, 300, 256});
    const auto [logits, boxes] = head->forward(inputs);
    assert(logits.sizes() == torch::IntArrayRef({2, 300, 91}));
    assert(boxes.sizes() == torch::IntArrayRef({2, 300, 4}));
    assert(boxes.min().item<float>() >= 0.0f);
    assert(boxes.max().item<float>() <= 1.0f);
}

void test_position_embedding_shape() {
    mmltk::rfdetr::PositionEmbeddingSine embedding(/*num_pos_feats=*/64, /*temperature=*/10000.0, /*normalize=*/true);
    mmltk::rfdetr::NestedTensor tensor{
        torch::zeros({2, 3, 16, 16}),
        torch::zeros({2, 16, 16}, torch::TensorOptions().dtype(torch::kBool)),
    };
    const auto output = embedding->forward(tensor);
    assert(output.sizes() == torch::IntArrayRef({2, 128, 16, 16}));

    const auto cached = embedding->forward_full_valid(/*batch_size=*/2, /*height=*/16, /*width=*/16, torch::kCPU);
    const auto cached_again = embedding->forward_full_valid(/*batch_size=*/2, /*height=*/16, /*width=*/16, torch::kCPU);
    assert(torch::allclose(output, cached, 1.0e-6, 1.0e-6));
    assert(torch::allclose(cached, cached_again, 1.0e-6, 1.0e-6));
    assert(cached.data_ptr<float>() == cached_again.data_ptr<float>());
}

void test_cuda_position_embedding_cache_is_stream_safe() {
    if (!torch::cuda::is_available()) {
        return;
    }

    const auto device = torch::Device(torch::kCUDA, 0);
    c10::cuda::CUDAGuard device_guard(static_cast<c10::DeviceIndex>(0));
    mmltk::rfdetr::PositionEmbeddingSine embedding(
        /*num_pos_feats=*/64,
        /*temperature=*/10000.0,
        /*normalize=*/true);

    const auto image_options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
    const auto mask_options = torch::TensorOptions().dtype(torch::kBool).device(device);
    mmltk::rfdetr::NestedTensor tensor{
        torch::zeros({2, 3, 16, 16}, image_options),
        torch::zeros({2, 16, 16}, mask_options),
    };
    const auto reference = embedding->forward(tensor).detach().clone();

    const auto stream0 = c10::cuda::getStreamFromPool(false, static_cast<c10::DeviceIndex>(0));
    const auto stream1 = c10::cuda::getStreamFromPool(false, static_cast<c10::DeviceIndex>(0));
    torch::Tensor cached_miss;
    torch::Tensor cached_hit;
    torch::Tensor cached_miss_copy;
    torch::Tensor cached_hit_copy;

    {
        c10::cuda::CUDAStreamGuard stream_guard(stream0);
        cached_miss = embedding->forward_full_valid(/*batch_size=*/2, /*height=*/16, /*width=*/16, device);
        cached_miss_copy = cached_miss.detach().clone();
    }
    {
        c10::cuda::CUDAStreamGuard stream_guard(stream1);
        cached_hit = embedding->forward_full_valid(/*batch_size=*/2, /*height=*/16, /*width=*/16, device);
        cached_hit_copy = cached_hit.detach().clone();
    }

    assert(cudaDeviceSynchronize() == cudaSuccess);
    assert(cached_miss.data_ptr<float>() == cached_hit.data_ptr<float>());
    assert(torch::allclose(reference, cached_miss_copy, 1.0e-6, 1.0e-6));
    assert(torch::allclose(reference, cached_hit_copy, 1.0e-6, 1.0e-6));
}

void test_segmentation_head_shapes() {
    mmltk::rfdetr::SegmentationHead head(/*in_dim=*/256, /*num_blocks=*/3);
    const auto spatial = torch::randn({2, 256, 16, 16});
    const std::vector<torch::Tensor> query_features = {
        torch::randn({2, 100, 256}),
        torch::randn({2, 100, 256}),
        torch::randn({2, 100, 256}),
    };

    const auto dense = head->forward(spatial, query_features, {64, 64});
    assert(dense.size() == 3);
    for (const auto& mask_logits : dense) {
        assert(mask_logits.sizes() == torch::IntArrayRef({2, 100, 16, 16}));
    }

    const auto sparse = head->sparse_forward(spatial, query_features, {64, 64});
    assert(sparse.size() == 3);
    for (const auto& item : sparse) {
        assert(item.spatial_features.sizes() == torch::IntArrayRef({2, 256, 16, 16}));
        assert(item.query_features.sizes() == torch::IntArrayRef({2, 100, 256}));
        assert(item.bias.sizes() == torch::IntArrayRef({1}));
    }

    const auto skip_dense = head->forward(spatial, {query_features.front()}, {64, 64}, /*skip_blocks=*/true);
    assert(skip_dense.size() == 1);
    assert(skip_dense.front().sizes() == torch::IntArrayRef({2, 100, 16, 16}));
}

void test_cuda_ms_deform_attn_matches_reference() {
    if (!torch::cuda::is_available()) {
        return;
    }

    const auto device = torch::Device(torch::kCUDA, 0);
    auto value = torch::randn(
        {2, 8, 2, 4},
        torch::TensorOptions().dtype(torch::kFloat32).device(device)).requires_grad_(true);
    auto spatial_shapes = torch::tensor(
        {{2, 2}, {1, 4}},
        torch::TensorOptions().dtype(torch::kInt64).device(device));
    auto level_start_index = torch::tensor(
        {0, 4},
        torch::TensorOptions().dtype(torch::kInt64).device(device));
    auto sampling_locations = torch::rand(
        {2, 3, 2, 2, 4, 2},
        torch::TensorOptions().dtype(torch::kFloat32).device(device)).requires_grad_(true);
    auto attention_weights = torch::rand(
        {2, 3, 2, 2, 4},
        torch::TensorOptions().dtype(torch::kFloat32).device(device));
    attention_weights = (attention_weights / attention_weights.sum({3, 4}, true)).detach().requires_grad_(true);

    auto value_ref = value.detach().clone().requires_grad_(true);
    auto sampling_ref = sampling_locations.detach().clone().requires_grad_(true);
    auto attention_ref = attention_weights.detach().clone().requires_grad_(true);

    const auto actual = mmltk::rfdetr::ms_deform_attn_cuda_autograd(
        value,
        spatial_shapes,
        level_start_index,
        sampling_locations,
        attention_weights,
        /*im2col_step=*/64);
    const auto expected = ms_deform_attn_reference(
        value_ref,
        spatial_shapes,
        sampling_ref,
        attention_ref);
    assert(torch::allclose(actual, expected, 1.0e-4, 1.0e-4));

    const auto grad = torch::randn_like(actual);
    (actual * grad).sum().backward();
    (expected * grad).sum().backward();

    assert(torch::allclose(value.grad(), value_ref.grad(), 2.0e-4, 2.0e-4));
    assert(torch::allclose(sampling_locations.grad(), sampling_ref.grad(), 2.0e-4, 2.0e-4));
    assert(torch::allclose(attention_weights.grad(), attention_ref.grad(), 2.0e-4, 2.0e-4));
}

} // namespace

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][modules]", test_detection_head_shapes);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][modules]", test_position_embedding_shape);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][modules]", test_cuda_position_embedding_cache_is_stream_safe);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][modules]", test_segmentation_head_shapes);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][modules]", test_cuda_ms_deform_attn_matches_reference);
