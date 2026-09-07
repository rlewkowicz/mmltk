#include <ATen/TensorIndexing.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include <torch/nn/functional.h>
#include <torch/torch.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "detail/detection_types.h"
#include "detail/model_technical.h"
#include "detail/modules_technical.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "torch_cuda_utils.h"

import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
import mmltk.backend.ml.cuda.gpu_quiescence;
import mmltk.backend.ml.cuda.shared_cuda_event;

namespace F = torch::nn::functional;

namespace mmltk::backend::models::rfdetr {

using mmltk::backend::ml::cuda::checked_device_index;
using mmltk::frameworks::gpu::ensure_cuda_ok;

struct PositionEmbeddingSineImpl::FullValidCache final {
    struct Entry final {
        c10::DeviceType device_type = c10::DeviceType::CPU;
        int device_index = -1;
        int64_t height = 0;
        int64_t width = 0;
        bool align_dim_orders = false;
        torch::Tensor base;
        std::optional<mmltk::backend::ml::cuda::CudaEventPool::Lease> ready_event;
    };

    class EventOwner final {
       public:
        explicit EventOwner(int device);
        void record_failure(cudaError_t failure) noexcept;
        [[nodiscard]] mmltk::backend::ml::cuda::CudaEventPool& pool() noexcept;
        [[nodiscard]] int device() const noexcept;

       private:
        int device_ = -1;
        std::atomic<cudaError_t> first_failure_{cudaSuccess};
        mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_owner_{1U};
        mmltk::backend::ml::cuda::CudaEventPool event_pool_;
    };

    [[nodiscard]] Entry* Find(int device_index, int64_t height, int64_t width, const torch::Device& device,
                              bool align_dim_orders) noexcept {
        for (auto& entry : entries) {
            if (entry.device_type == device.type() && entry.device_index == device_index && entry.height == height &&
                entry.width == width && entry.align_dim_orders == align_dim_orders) {
                return &entry;
            }
        }
        return nullptr;
    }

    std::mutex mutex;
    std::optional<EventOwner> event_owner;
    std::vector<Entry> entries;
};

namespace {

using namespace torch::indexing;

torch::Tensor build_sine_position_embedding(const torch::Tensor& x_embed, const torch::Tensor& y_embed, const int64_t num_pos_feats,
                                            const double temperature, const bool align_dim_orders, const bool make_contiguous) {
    auto dim_t = torch::arange(num_pos_feats, torch::TensorOptions().dtype(torch::kFloat32).device(x_embed.device()));
    dim_t = torch::pow(torch::full_like(dim_t, temperature), 2.0 * torch::floor(dim_t / 2.0) / static_cast<double>(num_pos_feats));

    auto pos_x = x_embed.unsqueeze(-1) / dim_t;
    auto pos_y = y_embed.unsqueeze(-1) / dim_t;
    pos_x = torch::stack(
                {
                    pos_x.index({Slice(), Slice(), Slice(), Slice(0, None, 2)}).sin(),
                    pos_x.index({Slice(), Slice(), Slice(), Slice(1, None, 2)}).cos(),
                },
                4)
                .flatten(3);
    pos_y = torch::stack(
                {
                    pos_y.index({Slice(), Slice(), Slice(), Slice(0, None, 2)}).sin(),
                    pos_y.index({Slice(), Slice(), Slice(), Slice(1, None, 2)}).cos(),
                },
                4)
                .flatten(3);

    auto pos = torch::cat({pos_y, pos_x}, 3);
    pos = align_dim_orders ? pos.permute({1, 2, 0, 3}) : pos.permute({0, 3, 1, 2});
    return make_contiguous ? pos.contiguous() : pos;
}

}  // namespace

torch::Tensor inverse_sigmoid(const torch::Tensor& x, double eps) {
    const auto clamped = x.clamp(0.0, 1.0);
    const auto x1 = clamped.clamp_min(eps);
    const auto x2 = (1.0 - clamped).clamp_min(eps);
    return torch::log(x1 / x2);
}

MlpImpl::MlpImpl(int64_t input_dim, int64_t hidden_dim, int64_t output_dim, int64_t num_layers)
    : num_layers_(num_layers), layers(register_module("layers", torch::nn::ModuleList())) {
    if (num_layers_ <= 0) { throw std::runtime_error("Mlp requires num_layers > 0"); }

    int64_t in_features = input_dim;
    for (int64_t index = 0; index < num_layers_; ++index) {
        const int64_t out_features = index + 1 == num_layers_ ? output_dim : hidden_dim;
        layers->push_back(torch::nn::Linear(in_features, out_features));
        in_features = hidden_dim;
    }
}

torch::Tensor MlpImpl::forward(torch::Tensor x) {
    for (int64_t index = 0; index < num_layers_; ++index) {
        x = layers[index]->as<torch::nn::Linear>()->forward(x);
        if (index + 1 < num_layers_) { x = torch::relu(x); }
    }
    return x;
}

DetectionHeadImpl::DetectionHeadImpl(int64_t hidden_dim, int64_t num_classes)
    : class_embed(register_module("class_embed", torch::nn::Linear(hidden_dim, num_classes))),
      bbox_embed(register_module("bbox_embed", Mlp(hidden_dim, hidden_dim, 4, 3))) {}

std::pair<torch::Tensor, torch::Tensor> DetectionHeadImpl::forward(const torch::Tensor& hs) {
    return {
        class_embed->forward(hs),
        bbox_embed->forward(hs).sigmoid(),
    };
}

PositionEmbeddingSineImpl::PositionEmbeddingSineImpl(int64_t num_pos_feats, double temperature, bool normalize, c10::optional<double> scale)
    : num_pos_feats_(num_pos_feats),
      temperature_(temperature),
      normalize_(normalize),
      scale_(scale.value_or(2.0 * M_PI)),
      full_valid_cache_(std::make_unique<FullValidCache>()) {
    if (scale.has_value() && !normalize_) { throw std::runtime_error("PositionEmbeddingSine scale requires normalize=true"); }
}

torch::Tensor PositionEmbeddingSineImpl::forward(const NestedTensor& tensor_list, bool align_dim_orders) const {
    if (!tensor_list.mask.defined()) { throw std::runtime_error("PositionEmbeddingSine requires a defined mask"); }
    return forward_mask(tensor_list.mask, align_dim_orders);
}

torch::Tensor PositionEmbeddingSineImpl::forward_mask(const torch::Tensor& mask, bool align_dim_orders) const {
    auto not_mask = torch::logical_not(mask);
    auto y_embed = not_mask.cumsum(1, torch::kFloat32);
    auto x_embed = not_mask.cumsum(2, torch::kFloat32);
    if (normalize_) {
        constexpr double kEps = 1e-6;
        y_embed = y_embed / (y_embed.index({Slice(), -1, Slice()}).unsqueeze(1) + kEps) * scale_;
        x_embed = x_embed / (x_embed.index({Slice(), Slice(), -1}).unsqueeze(2) + kEps) * scale_;
    }

    return build_sine_position_embedding(x_embed, y_embed, num_pos_feats_, temperature_, align_dim_orders, false);
}

torch::Tensor PositionEmbeddingSineImpl::forward_full_valid(int64_t batch_size, int64_t height, int64_t width, const torch::Device& device,
                                                            bool align_dim_orders) const {
    if (batch_size <= 0 || height <= 0 || width <= 0) {
        throw std::runtime_error("PositionEmbeddingSine full-valid cache requires positive batch and spatial sizes");
    }

    const int device_index = device.has_index() ? device.index() : -1;
    const auto wait_for_cache_entry = [](const auto& entry) {
        if (entry.device_type != c10::DeviceType::CUDA || !entry.ready_event) { return; }
        const auto cuda_device_index = checked_device_index(entry.device_index);
        c10::cuda::CUDAGuard device_guard(cuda_device_index);
        entry.ready_event->wait(reinterpret_cast<std::uintptr_t>(c10::cuda::getCurrentCUDAStream(cuda_device_index).stream()),
                                "wait for position embedding cache reuse");
    };
    const auto expand_entry = [batch_size](const auto& entry) {
        if (entry.align_dim_orders) { return entry.base.expand({entry.height, entry.width, batch_size, entry.base.size(3)}); }
        return entry.base.expand({batch_size, entry.base.size(1), entry.height, entry.width});
    };

    FullValidCache& cache = *full_valid_cache_;
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (auto* cached_entry = cache.Find(device_index, height, width, device, align_dim_orders)) {
        mmltk::common::logging::profile_add_value("rfdetr.pos_embed.cache_hit", 1);
        wait_for_cache_entry(*cached_entry);
        return expand_entry(*cached_entry);
    }

    mmltk::common::logging::profile_add_value("rfdetr.pos_embed.cache_miss", 1);
    torch::Tensor base;
    std::optional<mmltk::backend::ml::cuda::CudaEventPool::Lease> ready_event;
    if (device.is_cuda()) {
        const auto cuda_device_index = checked_device_index(device_index);
        c10::cuda::CUDAGuard device_guard(cuda_device_index);
        if (!cache.event_owner) cache.event_owner.emplace(device_index);
        if (cache.event_owner->device() != device_index) throw std::invalid_argument("position embedding cache has one fixed CUDA device");
        if (cache.entries.size() >= 16U) throw std::runtime_error("position embedding cache reached its fixed capacity");
        base = build_full_valid_base(height, width, device, align_dim_orders);
        ready_event =
            cache.event_owner->pool().record(reinterpret_cast<std::uintptr_t>(c10::cuda::getCurrentCUDAStream(cuda_device_index).stream()),
                                             "record position embedding cache readiness");
        if (!ready_event) throw std::runtime_error("position embedding cache event capacity is exhausted");
    } else {
        base = build_full_valid_base(height, width, device, align_dim_orders);
    }

    cache.entries.push_back(FullValidCache::Entry{
        device.type(),
        device_index,
        height,
        width,
        align_dim_orders,
        base,
        std::move(ready_event),
    });

    if (align_dim_orders) { return base.expand({height, width, batch_size, base.size(3)}); }
    return base.expand({batch_size, base.size(1), height, width});
}

PositionEmbeddingSineImpl::~PositionEmbeddingSineImpl() {
    if (!full_valid_cache_) return;
    FullValidCache& cache = *full_valid_cache_;
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (!cache.event_owner) return;
    const auto cuda_device_index = checked_device_index(cache.event_owner->device());
    c10::cuda::CUDAGuard device_guard(cuda_device_index);
    const auto settlement_stream = c10::cuda::getStreamFromPool(false, cuda_device_index);
    for (auto& entry : cache.entries) {
        if (entry.ready_event) {
            entry.ready_event->wait(reinterpret_cast<std::uintptr_t>(settlement_stream.stream()),
                                    "settle position embedding cache readiness");
        }
    }
    ensure_cuda_ok(cudaStreamSynchronize(settlement_stream.stream()), "settle position embedding cache event retirement");
    for (auto& entry : cache.entries) {
        if (entry.ready_event) {
            entry.ready_event->retire();
            entry.ready_event.reset();
        }
    }
}

torch::Tensor PositionEmbeddingSineImpl::build_full_valid_base(int64_t height, int64_t width, const torch::Device& device,
                                                               bool align_dim_orders) const {
    auto y_embed = torch::arange(1, height + 1, torch::TensorOptions().dtype(torch::kFloat32).device(device))
                       .view({1, height, 1})
                       .expand({1, height, width});
    auto x_embed = torch::arange(1, width + 1, torch::TensorOptions().dtype(torch::kFloat32).device(device))
                       .view({1, 1, width})
                       .expand({1, height, width});
    if (normalize_) {
        constexpr double kEps = 1e-6;
        y_embed = y_embed / (static_cast<double>(height) + kEps) * scale_;
        x_embed = x_embed / (static_cast<double>(width) + kEps) * scale_;
    }

    return build_sine_position_embedding(x_embed, y_embed, num_pos_feats_, temperature_, align_dim_orders, true);
}

PositionEmbeddingSineImpl::FullValidCache::EventOwner::EventOwner(const int device)
    : device_(device),
      event_pool_(mmltk::frameworks::gpu::make_cuda_device_owner<EventOwner, &EventOwner::record_failure>(this, device), 16U,
                  retirement_owner_) {}

void PositionEmbeddingSineImpl::FullValidCache::EventOwner::record_failure(const cudaError_t failure) noexcept {
    mmltk::frameworks::gpu::record_first_cuda_failure(first_failure_, failure);
}

mmltk::backend::ml::cuda::CudaEventPool& PositionEmbeddingSineImpl::FullValidCache::EventOwner::pool() noexcept { return event_pool_; }

int PositionEmbeddingSineImpl::FullValidCache::EventOwner::device() const noexcept { return device_; }

DepthwiseConvBlockImpl::DepthwiseConvBlockImpl(int64_t dim, double layer_scale_init_value)
    : dwconv(register_module("dwconv", torch::nn::Conv2d(torch::nn::Conv2dOptions(dim, dim, 3).padding(1).groups(dim)))),
      norm(register_module("norm", torch::nn::LayerNorm(torch::nn::LayerNormOptions({dim}).eps(1e-6)))),
      pwconv1(register_module("pwconv1", torch::nn::Linear(dim, dim))),
      act(register_module("act", torch::nn::GELU())) {
    if (layer_scale_init_value > 0.0) { gamma = register_parameter("gamma", torch::full({dim}, layer_scale_init_value, torch::kFloat32)); }
}

torch::Tensor DepthwiseConvBlockImpl::forward(torch::Tensor x) {
    const auto residual = x;
    x = dwconv->forward(x);
    x = x.permute({0, 2, 3, 1});
    x = norm->forward(x);
    x = pwconv1->forward(x);
    x = act->forward(x);
    if (gamma.defined()) { x = x * gamma; }
    x = x.permute({0, 3, 1, 2});
    return x + residual;
}

MlpBlockImpl::MlpBlockImpl(int64_t dim, double layer_scale_init_value)
    : norm_in(register_module("norm_in", torch::nn::LayerNorm(torch::nn::LayerNormOptions({dim})))),
      layers(register_module("layers", torch::nn::ModuleList())) {
    layers->push_back(torch::nn::Linear(dim, dim * 4));
    layers->push_back(torch::nn::GELU());
    layers->push_back(torch::nn::Linear(dim * 4, dim));

    if (layer_scale_init_value > 0.0) { gamma = register_parameter("gamma", torch::full({dim}, layer_scale_init_value, torch::kFloat32)); }
}

torch::Tensor MlpBlockImpl::forward(torch::Tensor x) {
    const auto residual = x;
    x = norm_in->forward(x);
    x = layers[0]->as<torch::nn::Linear>()->forward(x);
    x = layers[1]->as<torch::nn::GELU>()->forward(x);
    x = layers[2]->as<torch::nn::Linear>()->forward(x);
    if (gamma.defined()) { x = x * gamma; }
    return x + residual;
}

SegmentationHeadImpl::SegmentationHeadImpl(int64_t in_dim, int64_t num_blocks, c10::optional<int64_t> bottleneck_ratio,
                                           int64_t downsample_ratio)
    : blocks(register_module("blocks", torch::nn::ModuleList())),
      query_features_block(register_module("query_features_block", MlpBlock(in_dim))),
      bias(register_parameter("bias", torch::zeros({1}, torch::kFloat32))),
      downsample_ratio_(downsample_ratio),
      interaction_dim_(bottleneck_ratio.has_value() ? in_dim / *bottleneck_ratio : in_dim) {
    if (num_blocks < 0) { throw std::runtime_error("SegmentationHead requires num_blocks >= 0"); }
    if (downsample_ratio_ <= 0) { throw std::runtime_error("SegmentationHead requires downsample_ratio > 0"); }
    if (bottleneck_ratio.has_value() && *bottleneck_ratio <= 0) {
        throw std::runtime_error("SegmentationHead bottleneck_ratio must be positive when set");
    }

    for (int64_t index = 0; index < num_blocks; ++index) {
        blocks->push_back(DepthwiseConvBlock(in_dim));
    }

    if (bottleneck_ratio.has_value()) {
        spatial_features_proj =
            register_module("spatial_features_proj", torch::nn::Conv2d(torch::nn::Conv2dOptions(in_dim, interaction_dim_, 1)));
        query_features_proj = register_module("query_features_proj", torch::nn::Linear(in_dim, interaction_dim_));
    } else {
        use_spatial_identity_ = true;
        use_query_identity_ = true;
    }
}

torch::Tensor SegmentationHeadImpl::project_spatial_features(const torch::Tensor& spatial_features) {
    if (use_spatial_identity_) { return spatial_features; }
    return spatial_features_proj->forward(spatial_features);
}

torch::Tensor SegmentationHeadImpl::project_query_features(const torch::Tensor& query_features) {
    auto projected = query_features_block->forward(query_features);
    if (use_query_identity_) { return projected; }
    return query_features_proj->forward(projected);
}

torch::Tensor SegmentationHeadImpl::resize_spatial_features(const torch::Tensor& spatial_features,
                                                            std::pair<int64_t, int64_t> image_size) const {
    return F::interpolate(spatial_features, F::InterpolateFuncOptions()
                                                .size(std::vector<int64_t>{
                                                    image_size.first / downsample_ratio_,
                                                    image_size.second / downsample_ratio_,
                                                })
                                                .mode(torch::kBilinear)
                                                .align_corners(false));
}

template <typename Output, typename MakeOutput>
std::vector<Output> SegmentationHeadImpl::collect_head_outputs(const torch::Tensor& spatial_features,
                                                               const std::vector<torch::Tensor>& query_features,
                                                               const std::pair<int64_t, int64_t> image_size, const bool skip_blocks,
                                                               MakeOutput&& make_output) {
    auto resized_features = resize_spatial_features(spatial_features, image_size);
    std::vector<Output> outputs;
    outputs.reserve(query_features.size());

    if (!skip_blocks) {
        if (query_features.size() != blocks->size()) {
            throw std::runtime_error("SegmentationHead query_features size must match block count");
        }
        for (int64_t index = 0; index < static_cast<int64_t>(query_features.size()); ++index) {
            resized_features = blocks[index]->as<DepthwiseConvBlock>()->forward(resized_features);
            outputs.push_back(make_output(project_spatial_features(resized_features),
                                          project_query_features(query_features[static_cast<size_t>(index)])));
        }
        return outputs;
    }

    if (query_features.size() != 1) {
        throw std::runtime_error("SegmentationHead skip_blocks mode requires exactly one query feature tensor");
    }
    outputs.push_back(make_output(resized_features, project_query_features(query_features.front())));
    return outputs;
}

std::vector<torch::Tensor> SegmentationHeadImpl::forward(const torch::Tensor& spatial_features,
                                                         const std::vector<torch::Tensor>& query_features,
                                                         std::pair<int64_t, int64_t> image_size, bool skip_blocks) {
    return collect_head_outputs<torch::Tensor>(spatial_features, query_features, image_size, skip_blocks,
                                               [this](const torch::Tensor& spatial, const torch::Tensor& queries) {
                                                   return torch::einsum("bchw,bnc->bnhw", {spatial, queries}) + bias;
                                               });
}

std::vector<OutputLayer::SparsePredMasks> SegmentationHeadImpl::sparse_forward(const torch::Tensor& spatial_features,
                                                                               const std::vector<torch::Tensor>& query_features,
                                                                               std::pair<int64_t, int64_t> image_size, bool skip_blocks) {
    return collect_head_outputs<OutputLayer::SparsePredMasks>(
        spatial_features, query_features, image_size, skip_blocks, [this](torch::Tensor spatial, torch::Tensor queries) {
            return OutputLayer::SparsePredMasks{std::move(spatial), std::move(queries), bias};
        });
}

}  // namespace mmltk::backend::models::rfdetr
