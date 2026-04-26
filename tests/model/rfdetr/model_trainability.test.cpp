#include "mmltk/rfdetr/checkpoint.h"
#include "mmltk/rfdetr/detection_ops.h"
#include "mmltk/rfdetr/model.h"
#include "rfdetr/native_optimizer.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include <torch/csrc/autograd/autograd.h>

#include <algorithm>
#include "support/catch2_compat.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace mmltk::rfdetr {

bool is_muon_hidden_weight(std::string_view name, const torch::Tensor& param);

}

namespace {

template <typename T>
[[nodiscard]] const T& require_pointer(const T* value, const std::string_view message) {
    if (value == nullptr) {
        throw std::runtime_error(std::string(message));
    }
    return *value;
}

template <typename T>
[[nodiscard]] const T& require_optional_ref(const std::optional<T>& value, const std::string_view message) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(message));
    }
    return *value;
}

mmltk::rfdetr::PreparedTargets make_targets(int64_t batch_size, int64_t image_size) {
    mmltk::rfdetr::PreparedTargets targets;
    targets.orig_sizes = torch::full({batch_size, 2}, image_size, torch::TensorOptions().dtype(torch::kInt64));
    targets.nested_mask =
        torch::zeros({batch_size, image_size, image_size}, torch::TensorOptions().dtype(torch::kBool));

    std::vector<torch::Tensor> boxes;
    std::vector<torch::Tensor> labels;
    std::vector<torch::Tensor> area;
    std::vector<torch::Tensor> iscrowd;
    targets.offsets.reserve(static_cast<size_t>(batch_size));
    targets.counts.reserve(static_cast<size_t>(batch_size));
    for (int64_t index = 0; index < batch_size; ++index) {
        mmltk::rfdetr::PreparedTarget target;
        target.image_id = torch::tensor({index + 1}, torch::TensorOptions().dtype(torch::kInt64));
        target.orig_size = torch::tensor({image_size, image_size}, torch::TensorOptions().dtype(torch::kInt64));
        target.size = target.orig_size.clone();
        target.boxes = torch::tensor({{0.5f, 0.5f, 0.25f, 0.25f}}, torch::TensorOptions().dtype(torch::kFloat32));
        target.labels = torch::tensor({index % 2}, torch::TensorOptions().dtype(torch::kInt64));
        target.area = torch::tensor({64.0f}, torch::TensorOptions().dtype(torch::kFloat32));
        target.iscrowd = torch::zeros({1}, torch::TensorOptions().dtype(torch::kInt64));
        targets.offsets.push_back(index);
        targets.counts.push_back(1);
        boxes.push_back(target.boxes);
        labels.push_back(target.labels);
        area.push_back(target.area);
        iscrowd.push_back(target.iscrowd);
        targets.targets.push_back(std::move(target));
    }

    targets.all_boxes = torch::cat(boxes, 0);
    targets.all_labels = torch::cat(labels, 0);
    targets.all_area = torch::cat(area, 0);
    targets.all_iscrowd = torch::cat(iscrowd, 0);
    return targets;
}

mmltk::rfdetr::DetectionConfig make_detection_config(const mmltk::rfdetr::NativeRfDetrConfig& config) {
    mmltk::rfdetr::DetectionConfig detection_config;
    detection_config.num_classes = config.num_classes;
    detection_config.group_detr = config.group_detr;
    detection_config.dec_layers = config.dec_layers;
    detection_config.num_select = config.num_select;
    detection_config.aux_loss = true;
    detection_config.two_stage = config.two_stage;
    detection_config.include_masks = false;
    detection_config.cls_loss_coef = 2.0;
    detection_config.bbox_loss_coef = 5.0;
    detection_config.giou_loss_coef = 2.0;
    detection_config.set_cost_class = 2.0;
    detection_config.set_cost_bbox = 5.0;
    detection_config.set_cost_giou = 2.0;
    mmltk::rfdetr::populate_default_detection_weight_dict(detection_config);
    return detection_config;
}

mmltk::rfdetr::NativeCheckpoint make_checkpoint(const torch::nn::Module& module, int64_t num_classes,
                                                const char* preset_name = "unit-trainable") {
    mmltk::rfdetr::NativeCheckpoint checkpoint;
    checkpoint.metadata.preset_name = preset_name;
    checkpoint.metadata.source_kind = "native-test";
    checkpoint.metadata.source_path = "unit-test";
    checkpoint.metadata.num_classes = num_classes;
    for (const auto& item : module.named_parameters(true)) {
        checkpoint.state_dict.push_back({item.key(), item.value().detach().cpu().clone()});
    }
    for (const auto& item : module.named_buffers(true)) {
        checkpoint.state_dict.push_back({item.key(), item.value().detach().cpu().clone()});
    }
    return checkpoint;
}

torch::Tensor parameter_clone(const torch::nn::Module& module, const char* name) {
    const auto parameters = module.named_parameters(true);
    const auto& tensor = require_pointer(parameters.find(name), "missing named parameter for clone");
    return tensor.detach().cpu().clone();
}

torch::Tensor repeated_expected_rows(const torch::Tensor& source, int64_t rows) {
    const int64_t repeats = std::max<int64_t>(1, (rows + source.size(0) - 1) / source.size(0));
    std::vector<int64_t> repeat_dims(static_cast<size_t>(source.dim()), 1);
    repeat_dims[0] = repeats;
    return source.repeat(repeat_dims).narrow(0, 0, rows).contiguous();
}

void fill_linear_rows(torch::nn::Module& module, const char* weight_name, const char* bias_name, float row_base) {
    auto parameters = module.named_parameters(true);
    auto& weight = require_pointer(parameters.find(weight_name), "missing named weight parameter");
    auto& bias = require_pointer(parameters.find(bias_name), "missing named bias parameter");

    auto weight_cpu = torch::empty_like(weight.detach().cpu());
    auto bias_cpu = torch::empty_like(bias.detach().cpu());
    for (int64_t row = 0; row < weight_cpu.size(0); ++row) {
        const float row_value = row_base + static_cast<float>(row);
        weight_cpu.index_put_({row}, row_value);
        bias_cpu.index_put_({row}, row_value + 0.5f);
    }

    torch::NoGradGuard no_grad;
    weight.copy_(weight_cpu.to(weight.device(), weight.scalar_type()));
    bias.copy_(bias_cpu.to(bias.device(), bias.scalar_type()));
}

mmltk::rfdetr::NativeRfDetrConfig make_trainability_config(int64_t num_classes) {
    mmltk::rfdetr::NativeRfDetrConfig config;
    config.preset_name = "unit-trainable";
    config.resolution = 32;
    config.segmentation = false;
    config.num_classes = static_cast<int>(num_classes);
    config.num_queries = 8;
    config.num_select = 8;
    config.dec_layers = 2;
    config.group_detr = 1;
    config.two_stage = true;
    config.hidden_dim = 64;
    config.patch_size = 32;
    config.num_windows = 1;
    config.positional_encoding_size = 1;
    config.sa_nheads = 4;
    config.ca_nheads = 4;
    config.dec_n_points = 1;
    config.dim_feedforward = 128;
    return config;
}

mmltk::rfdetr::NativeAdamW make_optimizer(torch::nn::Module& module) {
    std::vector<mmltk::rfdetr::NativeAdamW::NamedParameter> params;
    params.reserve(module.named_parameters(true).size());
    for (const auto& item : module.named_parameters(true)) {
        if (item.value().requires_grad()) {
            params.push_back({item.key(), item.value()});
        }
    }
    const size_t param_count = params.size();
    std::vector<size_t> indices(param_count);
    for (size_t i = 0; i < param_count; ++i) {
        indices[i] = i;
    }
    return mmltk::rfdetr::NativeAdamW(
        {mmltk::rfdetr::NativeAdamW::Group{mmltk::rfdetr::NativeAdamWGroupConfig{1.0e-4, 1.0e-2, false},
                                           std::move(indices)}},
        std::move(params), mmltk::rfdetr::NativeOptimizerBackend::eager);
}

mmltk::rfdetr::NativeMuonWithAuxAdam make_muon_optimizer(torch::nn::Module& module) {
    std::vector<mmltk::rfdetr::NativeMuonWithAuxAdam::NamedParameter> params;
    params.reserve(module.named_parameters(true).size());
    std::vector<size_t> muon_indices;
    std::vector<size_t> aux_indices;
    for (const auto& item : module.named_parameters(true)) {
        if (!item.value().requires_grad()) {
            continue;
        }
        params.push_back({item.key(), item.value()});
        const size_t index = params.size() - 1;
        if (mmltk::rfdetr::is_muon_hidden_weight(item.key(), item.value())) {
            muon_indices.push_back(index);
        } else {
            aux_indices.push_back(index);
        }
    }

    std::vector<mmltk::rfdetr::NativeMuonWithAuxAdam::Group> groups;
    if (!muon_indices.empty()) {
        groups.push_back(mmltk::rfdetr::NativeMuonWithAuxAdam::Group{
            mmltk::rfdetr::NativeMuonGroupConfig{1.0e-4, 1.0e-2, 0.95, true, true}, std::move(muon_indices)});
    }
    if (!aux_indices.empty()) {
        groups.push_back(mmltk::rfdetr::NativeMuonWithAuxAdam::Group{
            mmltk::rfdetr::NativeMuonGroupConfig{1.0e-4, 1.0e-2, 0.95, false, true}, std::move(aux_indices)});
    }
    return {std::move(groups), std::move(params)};
}

torch::Tensor compute_cpu_train_loss(mmltk::rfdetr::NativeRfDetrModel& model, const mmltk::rfdetr::NestedTensor& batch,
                                     const mmltk::rfdetr::NativeRfDetrConfig& config) {
    model.train();
    auto outputs = model.forward(batch);
    const auto parameters = model.named_parameters(true);
    assert(parameters.find("transformer.enc_out_class_embed.0.weight") != nullptr);
    assert(parameters.find("transformer.enc_out_bbox_embed.0.layers.0.weight") != nullptr);
    const auto& enc_outputs =
        require_optional_ref(outputs.enc_outputs, "expected encoder outputs during train loss computation");
    assert(enc_outputs.pred_logits.defined());
    assert(enc_outputs.pred_boxes.defined());
    assert(enc_outputs.pred_logits.size(0) == 2);
    assert(enc_outputs.pred_boxes.size(-1) == 4);
    const auto targets = make_targets(2, 32);
    const auto detection_config = make_detection_config(config);
    auto loss_dict = mmltk::rfdetr::detection_loss_dict(outputs, targets, detection_config, true, false);
    auto loss = mmltk::rfdetr::weighted_detection_loss(loss_dict, detection_config, torch::Device(torch::kCPU));
    assert(torch::isfinite(loss).item<bool>());
    return loss;
}

void run_cpu_train_step(mmltk::rfdetr::NativeRfDetrModel& model, const mmltk::rfdetr::NestedTensor& batch,
                        const mmltk::rfdetr::NativeRfDetrConfig& config) {
    auto loss = compute_cpu_train_loss(model, batch, config);
    auto optimizer = make_optimizer(model);
    loss.backward();
    optimizer.step();
    optimizer.zero_grad(true);
}

void run_cpu_muon_train_step(mmltk::rfdetr::NativeRfDetrModel& model, const mmltk::rfdetr::NestedTensor& batch,
                             const mmltk::rfdetr::NativeRfDetrConfig& config) {
    auto loss = compute_cpu_train_loss(model, batch, config);
    auto optimizer = make_muon_optimizer(model);
    loss.backward();
    optimizer.step();
    optimizer.zero_grad(true);
}

mmltk::rfdetr::NestedTensor make_cpu_batch() {
    return mmltk::rfdetr::NestedTensor{
        torch::rand({2, 3, 32, 32}, torch::TensorOptions().dtype(torch::kFloat32)),
        torch::zeros({2, 32, 32}, torch::TensorOptions().dtype(torch::kBool)),
    };
}

fs::path make_temp_checkpoint_path(const std::string_view file_name) {
    return fs::temp_directory_path() / std::string(file_name);
}

void prepare_model_for_training(mmltk::rfdetr::NativeRfDetrModel& model,
                                const std::optional<torch::Device>& device = std::nullopt) {
    if (device.has_value()) {
        model.to(*device);
    }
    model.train();
}

template <typename SummaryT>
void assert_checkpoint_summary_has_no_mismatches(const SummaryT& summary, const bool require_loaded_names = true) {
    if (require_loaded_names) {
        assert(!summary.loaded_names.empty());
    }
    assert(summary.missing_names.empty());
    assert(summary.unexpected_names.empty());
    assert(summary.incompatible_names.empty());
}

void assert_checkpoint_roundtrip(const mmltk::rfdetr::NativeRfDetrConfig& config, const torch::nn::Module& model) {
    const fs::path checkpoint_path = make_temp_checkpoint_path("mmltk_rfdetr_model_trainability.pt");
    mmltk::rfdetr::save_native_checkpoint(checkpoint_path, make_checkpoint(model, config.num_classes));
    const auto loaded = mmltk::rfdetr::load_checkpoint(checkpoint_path);
    assert(loaded.metadata.num_classes == config.num_classes);

    auto reloaded = mmltk::rfdetr::NativeRfDetrModel(config);
    const auto summary = mmltk::rfdetr::apply_checkpoint_to_module(reloaded, checkpoint_path, true);
    assert_checkpoint_summary_has_no_mismatches(summary);

    fs::remove(checkpoint_path);
}

mmltk::rfdetr::PreparedTargets make_targets_on_device(int64_t batch_size, int64_t image_size,
                                                      const torch::Device& device) {
    auto targets = make_targets(batch_size, image_size);
    targets.orig_sizes = targets.orig_sizes.to(device);
    targets.nested_mask = targets.nested_mask.to(device);
    targets.all_boxes = targets.all_boxes.to(device);
    targets.all_labels = targets.all_labels.to(device);
    targets.all_area = targets.all_area.to(device);
    targets.all_iscrowd = targets.all_iscrowd.to(device);
    for (auto& target : targets.targets) {
        target.image_id = target.image_id.to(device);
        target.orig_size = target.orig_size.to(device);
        target.size = target.size.to(device);
        target.boxes = target.boxes.to(device);
        target.labels = target.labels.to(device);
        target.area = target.area.to(device);
        target.iscrowd = target.iscrowd.to(device);
    }
    return targets;
}

mmltk::rfdetr::NestedTensor make_cuda_batch(int64_t batch_size, int64_t image_size, float offset,
                                            const torch::Device& device) {
    auto values = torch::arange(batch_size * 3 * image_size * image_size,
                                torch::TensorOptions().dtype(torch::kFloat32).device(device));
    values = values.view({batch_size, 3, image_size, image_size});
    values = values.div(static_cast<double>(batch_size * 3 * image_size * image_size)).add(offset);
    return mmltk::rfdetr::NestedTensor{
        values.contiguous(),
        torch::zeros({batch_size, image_size, image_size}, torch::TensorOptions().dtype(torch::kBool).device(device)),
    };
}

struct CudaTrainInputs {
    mmltk::rfdetr::NestedTensor batch;
    mmltk::rfdetr::PreparedTargets targets;
    mmltk::rfdetr::DetectionConfig detection_config;
};

CudaTrainInputs make_cuda_train_inputs(const mmltk::rfdetr::NativeRfDetrConfig& config, const torch::Device& device,
                                       int64_t batch_size = 2, int64_t image_size = 32) {
    return CudaTrainInputs{
        mmltk::rfdetr::NestedTensor{
            torch::rand({batch_size, 3, image_size, image_size},
                        torch::TensorOptions().dtype(torch::kFloat32).device(device)),
            torch::zeros({batch_size, image_size, image_size},
                         torch::TensorOptions().dtype(torch::kBool).device(device)),
        },
        make_targets_on_device(batch_size, image_size, device),
        make_detection_config(config),
    };
}

CudaTrainInputs make_offset_cuda_train_inputs(const mmltk::rfdetr::NativeRfDetrConfig& config,
                                              const torch::Device& device, int64_t batch_size, int64_t image_size,
                                              float offset) {
    return CudaTrainInputs{
        make_cuda_batch(batch_size, image_size, offset, device),
        make_targets_on_device(batch_size, image_size, device),
        make_detection_config(config),
    };
}

void copy_module_state(torch::nn::Module& destination, const torch::nn::Module& source) {
    torch::NoGradGuard no_grad;
    auto destination_parameters = destination.named_parameters(true);
    for (const auto& item : source.named_parameters(true)) {
        auto& tensor = require_pointer(destination_parameters.find(item.key()),
                                       "missing destination parameter while copying state");
        tensor.copy_(item.value());
    }

    auto destination_buffers = destination.named_buffers(true);
    for (const auto& item : source.named_buffers(true)) {
        auto& tensor =
            require_pointer(destination_buffers.find(item.key()), "missing destination buffer while copying state");
        tensor.copy_(item.value());
    }
}

std::vector<torch::Tensor> parameter_refs(const torch::nn::Module& module, const std::vector<std::string>& names) {
    const auto named_parameters = module.named_parameters(true);
    std::vector<torch::Tensor> parameters;
    parameters.reserve(names.size());
    for (const auto& name : names) {
        parameters.push_back(
            require_pointer(named_parameters.find(name), "missing named parameter while collecting parameter refs"));
    }
    return parameters;
}

void assert_modules_allclose(const torch::nn::Module& lhs, const torch::nn::Module& rhs, double atol, double rtol) {
    const auto lhs_parameters = lhs.named_parameters(true);
    const auto rhs_parameters = rhs.named_parameters(true);
    assert(lhs_parameters.size() == rhs_parameters.size());
    for (const auto& item : lhs_parameters) {
        const auto& rhs_tensor =
            require_pointer(rhs_parameters.find(item.key()), "missing rhs parameter during allclose comparison");
        const auto lhs_cpu = item.value().detach().to(torch::kCPU).contiguous();
        const auto rhs_cpu = rhs_tensor.detach().to(torch::kCPU).contiguous();
        if (torch::allclose(lhs_cpu, rhs_cpu, rtol, atol)) {
            continue;
        }

        const auto abs_diff = (lhs_cpu - rhs_cpu).abs();
        const auto max_abs = abs_diff.max().item<double>();
        const auto scale = torch::maximum(lhs_cpu.abs(), rhs_cpu.abs());
        const auto max_rel = (abs_diff / torch::clamp_min(scale, static_cast<double>(atol))).max().item<double>();
        throw std::runtime_error(std::string("module parameter mismatch for ") + item.key() +
                                 ": max_abs=" + std::to_string(max_abs) + " max_rel=" + std::to_string(max_rel));
    }
}

void cuda_check(cudaError_t status, const char* context) {
    if (status != cudaSuccess) {
        std::fprintf(stderr, "%s: %s\n", context, cudaGetErrorString(status));
        std::abort();
    }
}

template <typename MakeOptimizerFn>
void run_cuda_train_step_smoke(MakeOptimizerFn make_optimizer_fn) {
    if (!torch::cuda::is_available()) {
        return;
    }

    const auto device = torch::Device(torch::kCUDA, 0);
    const auto config = make_trainability_config(3);
    auto model = mmltk::rfdetr::NativeRfDetrModel(config);
    prepare_model_for_training(model, device);
    const auto inputs = make_cuda_train_inputs(config, device);

    auto optimizer = make_optimizer_fn(model);
    auto outputs = model.forward(inputs.batch);
    auto loss_dict = mmltk::rfdetr::detection_loss_dict(outputs, inputs.targets, inputs.detection_config, true, false);
    auto loss = mmltk::rfdetr::weighted_detection_loss(loss_dict, inputs.detection_config, device);
    assert(torch::isfinite(loss).item<bool>());
    loss.backward();
    optimizer.step();
    optimizer.zero_grad(true);
}

void test_train_step_only() {
    const auto config = make_trainability_config(3);
    auto model = mmltk::rfdetr::NativeRfDetrModel(config);
    auto batch = make_cpu_batch();
    run_cpu_train_step(model, batch, config);
}

void test_muon_train_step_only() {
    const auto config = make_trainability_config(3);
    auto model = mmltk::rfdetr::NativeRfDetrModel(config);
    auto batch = make_cpu_batch();
    run_cpu_muon_train_step(model, batch, config);
}

void test_train_forward_loss_only() {
    const auto config = make_trainability_config(3);
    auto model = mmltk::rfdetr::NativeRfDetrModel(config);
    auto batch = make_cpu_batch();
    auto loss = compute_cpu_train_loss(model, batch, config);
    assert(torch::isfinite(loss).item<bool>());
}

void test_train_backward_only() {
    const auto config = make_trainability_config(3);
    auto model = mmltk::rfdetr::NativeRfDetrModel(config);
    auto batch = make_cpu_batch();
    auto loss = compute_cpu_train_loss(model, batch, config);
    loss.backward();
}

void test_checkpoint_roundtrip_only() {
    const auto config = make_trainability_config(3);
    auto model = mmltk::rfdetr::NativeRfDetrModel(config);
    prepare_model_for_training(model);
    assert_checkpoint_roundtrip(config, model);
}

void test_shared_model_concurrent_eval_only() {
    const auto config = make_trainability_config(3);
    auto model = std::make_shared<mmltk::rfdetr::NativeRfDetrModel>(config);
    model->eval();
    const auto batch = make_cpu_batch();

    struct ForwardSnapshot {
        torch::Tensor logits;
        torch::Tensor boxes;
    };
    const auto run_forward = [model, batch]() {
        torch::NoGradGuard no_grad;
        auto outputs = model->forward(batch);
        assert(outputs.main.pred_logits.defined());
        assert(outputs.main.pred_boxes.defined());
        return ForwardSnapshot{
            outputs.main.pred_logits.detach().clone(),
            outputs.main.pred_boxes.detach().clone(),
        };
    };

    auto lhs = std::async(std::launch::async, run_forward);
    auto rhs = std::async(std::launch::async, run_forward);
    const auto lhs_snapshot = lhs.get();
    const auto rhs_snapshot = rhs.get();
    assert(torch::equal(lhs_snapshot.logits, rhs_snapshot.logits));
    assert(torch::equal(lhs_snapshot.boxes, rhs_snapshot.boxes));
}

void test_train_step_and_checkpoint_roundtrip() {
    const auto config = make_trainability_config(3);
    auto model = mmltk::rfdetr::NativeRfDetrModel(config);
    auto batch = make_cpu_batch();
    run_cpu_train_step(model, batch, config);
    assert_checkpoint_roundtrip(config, model);
}

void test_cuda_train_step_smoke() {
    run_cuda_train_step_smoke([](torch::nn::Module& model) { return make_optimizer(model); });
}

void test_cuda_muon_train_step_smoke() {
    run_cuda_train_step_smoke([](torch::nn::Module& model) { return make_muon_optimizer(model); });
}

void test_cuda_parallel_wave_matches_sequential_accumulation() {
    if (!torch::cuda::is_available()) {
        return;
    }

    const auto device = torch::Device(torch::kCUDA, 0);
    c10::cuda::CUDAGuard device_guard(static_cast<c10::DeviceIndex>(0));
    const auto config = make_trainability_config(3);

    auto sequential_model = mmltk::rfdetr::NativeRfDetrModel(config);
    prepare_model_for_training(sequential_model, device);

    auto parallel_model = mmltk::rfdetr::NativeRfDetrModel(config);
    prepare_model_for_training(parallel_model, device);
    copy_module_state(parallel_model, sequential_model);

    auto lane0_model = mmltk::rfdetr::NativeRfDetrModel(config);
    prepare_model_for_training(lane0_model, device);
    copy_module_state(lane0_model, parallel_model);

    auto lane1_model = mmltk::rfdetr::NativeRfDetrModel(config);
    prepare_model_for_training(lane1_model, device);
    copy_module_state(lane1_model, parallel_model);

    auto sequential_optimizer = make_optimizer(sequential_model);
    auto parallel_optimizer = make_optimizer(parallel_model);
    auto& parallel_params = parallel_optimizer.parameters();
    const auto& parallel_param_names = parallel_optimizer.parameter_names();
    auto lane0_params = parameter_refs(lane0_model, parallel_param_names);
    auto lane1_params = parameter_refs(lane1_model, parallel_param_names);

    const auto inputs_a = make_offset_cuda_train_inputs(config, device, 2, 32, 0.0f);
    const auto inputs_b = make_offset_cuda_train_inputs(config, device, 2, 32, 1.0f);

    torch::manual_seed(42);
    {
        auto outputs_a = sequential_model.forward(inputs_a.batch);
        auto loss_dict_a =
            mmltk::rfdetr::detection_loss_dict(outputs_a, inputs_a.targets, inputs_a.detection_config, true, false);
        auto loss_a = mmltk::rfdetr::weighted_detection_loss(loss_dict_a, inputs_a.detection_config, device);
        (loss_a / 2.0).backward();

        auto outputs_b = sequential_model.forward(inputs_b.batch);
        auto loss_dict_b =
            mmltk::rfdetr::detection_loss_dict(outputs_b, inputs_b.targets, inputs_b.detection_config, true, false);
        auto loss_b = mmltk::rfdetr::weighted_detection_loss(loss_dict_b, inputs_b.detection_config, device);
        (loss_b / 2.0).backward();

        sequential_optimizer.step();
        sequential_optimizer.zero_grad(true);
    }

    torch::manual_seed(42);
    std::vector<torch::Tensor> lane0_grads;
    std::vector<torch::Tensor> lane1_grads;
    const auto stream0 = c10::cuda::getStreamFromPool(false, static_cast<c10::DeviceIndex>(0));
    const auto stream1 = c10::cuda::getStreamFromPool(false, static_cast<c10::DeviceIndex>(0));
    cudaEvent_t lane0_ready = nullptr;
    cudaEvent_t lane1_ready = nullptr;
    cuda_check(cudaEventCreateWithFlags(&lane0_ready, cudaEventDisableTiming), "cudaEventCreateWithFlags lane0");
    cuda_check(cudaEventCreateWithFlags(&lane1_ready, cudaEventDisableTiming), "cudaEventCreateWithFlags lane1");

    {
        c10::cuda::CUDAStreamGuard stream_guard(stream0);
        auto outputs = lane0_model.forward(inputs_a.batch);
        auto loss_dict =
            mmltk::rfdetr::detection_loss_dict(outputs, inputs_a.targets, inputs_a.detection_config, true, false);
        auto loss = mmltk::rfdetr::weighted_detection_loss(loss_dict, inputs_a.detection_config, device);
        lane0_grads = torch::autograd::grad({loss / 2.0}, lane0_params, {}, std::nullopt, false, true);
        cuda_check(cudaEventRecord(lane0_ready, stream0.stream()), "cudaEventRecord lane0");
    }
    {
        c10::cuda::CUDAStreamGuard stream_guard(stream1);
        auto outputs = lane1_model.forward(inputs_b.batch);
        auto loss_dict =
            mmltk::rfdetr::detection_loss_dict(outputs, inputs_b.targets, inputs_b.detection_config, true, false);
        auto loss = mmltk::rfdetr::weighted_detection_loss(loss_dict, inputs_b.detection_config, device);
        lane1_grads = torch::autograd::grad({loss / 2.0}, lane1_params, {}, std::nullopt, false, true);
        cuda_check(cudaEventRecord(lane1_ready, stream1.stream()), "cudaEventRecord lane1");
    }
    cuda_check(
        cudaStreamWaitEvent(c10::cuda::getCurrentCUDAStream(static_cast<c10::DeviceIndex>(0)).stream(), lane0_ready, 0),
        "cudaStreamWaitEvent lane0");
    cuda_check(
        cudaStreamWaitEvent(c10::cuda::getCurrentCUDAStream(static_cast<c10::DeviceIndex>(0)).stream(), lane1_ready, 0),
        "cudaStreamWaitEvent lane1");
    {
        torch::NoGradGuard no_grad;
        for (size_t index = 0; index < parallel_params.size(); ++index) {
            if (index < lane0_grads.size() && lane0_grads[index].defined()) {
                parallel_params[index].mutable_grad() = lane0_grads[index].detach().clone();
            }
            if (index < lane1_grads.size() && lane1_grads[index].defined()) {
                if (!parallel_params[index].grad().defined()) {
                    parallel_params[index].mutable_grad() = lane1_grads[index].detach().clone();
                } else {
                    parallel_params[index].mutable_grad().add_(lane1_grads[index]);
                }
            }
        }
    }
    parallel_optimizer.step();
    parallel_optimizer.zero_grad(true);

    cuda_check(cudaDeviceSynchronize(), "cudaDeviceSynchronize before event destroy");
    cuda_check(cudaEventDestroy(lane0_ready), "cudaEventDestroy lane0");
    cuda_check(cudaEventDestroy(lane1_ready), "cudaEventDestroy lane1");
    assert_modules_allclose(sequential_model, parallel_model, 1.0e-4, 1.0e-4);
}

void test_train_model_has_no_running_stats_buffers() {
    const auto config = make_trainability_config(3);
    auto model = mmltk::rfdetr::NativeRfDetrModel(config);
    for (const auto& item : model.named_buffers(true)) {
        const auto& name = item.key();
        assert(name.find("running_mean") == std::string::npos);
        assert(name.find("running_var") == std::string::npos);
        assert(name.find("num_batches_tracked") == std::string::npos);
    }
}

void test_checkpoint_load_with_class_head_resize(int64_t target_classes, int64_t checkpoint_classes,
                                                 const char* checkpoint_suffix) {
    auto target_config = make_trainability_config(target_classes);
    target_config.preset_name = "rf-detr-nano";

    auto checkpoint_config = target_config;
    checkpoint_config.num_classes = static_cast<int>(checkpoint_classes);

    auto checkpoint_model = mmltk::rfdetr::NativeRfDetrModel(checkpoint_config);
    fill_linear_rows(checkpoint_model, "class_embed.weight", "class_embed.bias", 10.0f);
    fill_linear_rows(checkpoint_model, "transformer.enc_out_class_embed.0.weight",
                     "transformer.enc_out_class_embed.0.bias", 20.0f);
    const auto checkpoint_class_weight = parameter_clone(checkpoint_model, "class_embed.weight");
    const auto checkpoint_class_bias = parameter_clone(checkpoint_model, "class_embed.bias");
    const auto checkpoint_enc_weight = parameter_clone(checkpoint_model, "transformer.enc_out_class_embed.0.weight");
    const auto checkpoint_enc_bias = parameter_clone(checkpoint_model, "transformer.enc_out_class_embed.0.bias");

    const fs::path checkpoint_path =
        make_temp_checkpoint_path("mmltk_rfdetr_model_resize_" + std::string(checkpoint_suffix) + ".native.pt");
    mmltk::rfdetr::save_native_checkpoint(
        checkpoint_path, make_checkpoint(checkpoint_model, checkpoint_config.num_classes, "rf-detr-nano"));

    auto target_model = mmltk::rfdetr::NativeRfDetrModel(target_config);
    const auto summary = target_model.load_weights(checkpoint_path, false);
    assert_checkpoint_summary_has_no_mismatches(summary, false);

    const auto expected_class_weight = repeated_expected_rows(checkpoint_class_weight, target_config.num_classes);
    const auto expected_class_bias = repeated_expected_rows(checkpoint_class_bias, target_config.num_classes);
    const auto expected_enc_weight = repeated_expected_rows(checkpoint_enc_weight, target_config.num_classes);
    const auto expected_enc_bias = repeated_expected_rows(checkpoint_enc_bias, target_config.num_classes);

    const auto loaded_class_weight = parameter_clone(target_model, "class_embed.weight");
    const auto loaded_class_bias = parameter_clone(target_model, "class_embed.bias");
    const auto loaded_enc_weight = parameter_clone(target_model, "transformer.enc_out_class_embed.0.weight");
    const auto loaded_enc_bias = parameter_clone(target_model, "transformer.enc_out_class_embed.0.bias");

    assert(loaded_class_weight.size(0) == target_config.num_classes);
    assert(loaded_class_bias.size(0) == target_config.num_classes);
    assert(loaded_enc_weight.size(0) == target_config.num_classes);
    assert(loaded_enc_bias.size(0) == target_config.num_classes);
    assert(torch::equal(loaded_class_weight, expected_class_weight));
    assert(torch::equal(loaded_class_bias, expected_class_bias));
    assert(torch::equal(loaded_enc_weight, expected_enc_weight));
    assert(torch::equal(loaded_enc_bias, expected_enc_bias));

    fs::remove(checkpoint_path);
}

}  // namespace

void test_checkpoint_load_with_class_head_resize_shrink() {
    test_checkpoint_load_with_class_head_resize(3, 7, "shrink");
}

void test_checkpoint_load_with_class_head_resize_widen() {
    test_checkpoint_load_with_class_head_resize(7, 3, "widen");
}

MMLTK_REGISTER_TEST_CASE("[.][model][rfdetr][model_trainability][optin]", test_train_forward_loss_only);
MMLTK_REGISTER_TEST_CASE("[.][model][rfdetr][model_trainability][optin]", test_train_backward_only);
MMLTK_REGISTER_TEST_CASE("[.][model][rfdetr][model_trainability][optin]", test_train_step_only);
MMLTK_REGISTER_TEST_CASE("[.][model][rfdetr][model_trainability][optin]", test_muon_train_step_only);
MMLTK_REGISTER_TEST_CASE("[.][model][rfdetr][model_trainability][optin]", test_checkpoint_roundtrip_only);

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][model_trainability]", test_train_step_and_checkpoint_roundtrip);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][model_trainability]", test_shared_model_concurrent_eval_only);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][model_trainability]", test_train_model_has_no_running_stats_buffers);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][model_trainability][cuda]", test_cuda_train_step_smoke);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][model_trainability][cuda]", test_cuda_muon_train_step_smoke);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][model_trainability][cuda]",
                         test_cuda_parallel_wave_matches_sequential_accumulation);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][model_trainability]", test_checkpoint_load_with_class_head_resize_shrink);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][model_trainability]", test_checkpoint_load_with_class_head_resize_widen);
