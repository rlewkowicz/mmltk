module;
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"

#define ONNX_NAMESPACE onnx_torch

#include <ATen/Context.h>
#include <cuda_runtime_api.h>
#include <torch/csrc/jit/frontend/tracer.h>
#include <torch/csrc/jit/serialization/export.h>
#include <torch/csrc/onnx/onnx.h>
#include <torch/script.h>

#undef ONNX_NAMESPACE

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "model_technical.h"

module mmltk.backend.models.rfdetr.model_export;

import mmltk.backend.ml.cuda.torch_scope;
import mmltk.backend.models.rfdetr.core.model;

import mmltk.backend.models.rfdetr.model_export.onnx_lowering;
import :onnx_simplify;

import mmltk.common.logging.mmltk_logging;

#include "model_access.h"
#include "model_state_access.h"

namespace mmltk::backend::models::rfdetr {
namespace runtime = mmltk::backend::ml::runtime;
namespace torch_cuda = mmltk::backend::ml::cuda;
namespace {

std::mutex& sdp_backend_mutex() {
    static std::mutex mutex;
    return mutex;
}

class ScopedSdpBackend final {
   public:
    ScopedSdpBackend()
        : lock_(sdp_backend_mutex()),
          flash_(at::globalContext().userEnabledFlashSDP()),
          memory_efficient_(at::globalContext().userEnabledMemEfficientSDP()),
          math_(at::globalContext().userEnabledMathSDP()),
          cudnn_(at::globalContext().userEnabledCuDNNSDP()) {
        auto& context = at::globalContext();
        context.setSDPUseFlash(false);
        context.setSDPUseMemEfficient(false);
        context.setSDPUseMath(true);
        context.setSDPUseCuDNN(false);
    }

    ~ScopedSdpBackend() {
        auto& context = at::globalContext();
        context.setSDPUseFlash(flash_);
        context.setSDPUseMemEfficient(memory_efficient_);
        context.setSDPUseMath(math_);
        context.setSDPUseCuDNN(cudnn_);
    }

   private:
    std::unique_lock<std::mutex> lock_;
    bool flash_;
    bool memory_efficient_;
    bool math_;
    bool cudnn_;
};

void erase_unused_module_self_input(const std::shared_ptr<torch::jit::Graph>& graph) {
    if (!graph->inputs().empty() && graph->inputs().front()->type()->kind() == c10::TypeKind::ClassType) { graph->eraseInput(0); }
}

void assign_onnx_tensor_names(const std::shared_ptr<torch::jit::Graph>& graph, const bool has_masks) {
    constexpr std::array<std::string_view, 3> output_names{"pred_logits", "pred_boxes", "pred_masks"};
    const std::size_t expected_outputs = has_masks ? output_names.size() : output_names.size() - 1U;
    if (graph->inputs().size() != 1U || graph->outputs().size() != expected_outputs) {
        throw std::runtime_error("RF-DETR ONNX graph does not expose the expected image and prediction tensors");
    }

    graph->inputs().front()->setDebugName("pixel_values");
    for (std::size_t index = 0U; index < expected_outputs; ++index) {
        graph->outputs()[index]->setDebugName(std::string(output_names[index]));
    }
}

}  // namespace

void export_model_onnx(NativeRfDetrModel& model, const std::filesystem::path& output_path, const int opset_version, const int batch_size,
                       const bool simplify, const std::filesystem::path& explicit_descriptor, const std::stop_token stop) {
    if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
    validate_supported_onnx_export_opset(opset_version);
    ClassArtifactPublication publication(output_path, explicit_descriptor);
    if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
    const auto& staged_model = publication.staged_artifact();
    auto& technical_model = detail::native_model_owner(model);
    technical_model.module().eval();
    technical_model.set_force_pytorch_deformable_attn(true);
    const auto restore_attention = [&technical_model] { technical_model.set_force_pytorch_deformable_attn(false); };
    try {
        const torch::Tensor reference = technical_model.module().parameters().front();
        const torch::Tensor dummy_pixels =
            torch::zeros({batch_size, 3, model.config().resolution, model.config().resolution}, reference.options());
        const torch::Tensor dummy_mask = torch::zeros({batch_size, model.config().resolution, model.config().resolution},
                                                      torch::TensorOptions().dtype(torch::kBool).device(reference.device()));

        auto compilation_unit = std::make_shared<torch::jit::CompilationUnit>();
        auto class_type = torch::jit::ClassType::create("__torch__.NativeRfDetrOnnxExport", compilation_unit, true);
        torch::jit::Module export_module(compilation_unit, class_type);
        for (const auto& parameter : technical_model.module().named_parameters(true)) {
            std::string name = parameter.key();
            std::replace(name.begin(), name.end(), '.', '_');
            export_module.register_parameter(name, parameter.value(), false);
        }
        for (const auto& buffer : technical_model.module().named_buffers(true)) {
            std::string name = buffer.key();
            std::replace(name.begin(), name.end(), '.', '_');
            export_module.register_buffer(name, buffer.value());
        }

        const bool has_masks = model.config().segmentation;
        ScopedSdpBackend sdp_scope;
        auto traced = torch::jit::tracer::trace(
            {dummy_pixels},
            [&technical_model, dummy_mask, has_masks](torch::jit::Stack args) {
                const auto outputs = technical_model.forward(NestedTensor{args[0].toTensor(), dummy_mask}, true);
                if (has_masks && outputs.main.pred_masks) {
                    return torch::jit::Stack{outputs.main.pred_logits, outputs.main.pred_boxes, *outputs.main.pred_masks};
                }
                return torch::jit::Stack{outputs.main.pred_logits, outputs.main.pred_boxes};
            },
            [](const torch::autograd::Variable&) { return ""; }, false, false, &export_module);
        export_module.type()->addMethod(compilation_unit->create_function("forward", traced.first->graph, true));

        std::unordered_map<std::string, at::Tensor> lowering_initializers;
        const auto collect = [&](const std::string& name, const at::Tensor& value) { lowering_initializers.emplace(name, value); };
        for (const auto& tensor : export_module.named_parameters(true)) {
            collect(tensor.name, tensor.value);
        }
        for (const auto& tensor : export_module.named_buffers(true)) {
            collect(tensor.name, tensor.value);
        }
        const auto graph = traced.first->graph;
        lower_graph_for_onnx_export({
            .graph = graph.get(),
            .initializer_context = &lowering_initializers,
            .find_initializer = [](const void* opaque, const std::string& name) noexcept -> const void* {
                const auto& values = *static_cast<const std::unordered_map<std::string, at::Tensor>*>(opaque);
                const auto found = values.find(name);
                return found == values.end() ? nullptr : std::addressof(found->second);
            },
        });
        erase_unused_module_self_input(graph);
        assign_onnx_tensor_names(graph, has_masks);
        std::unordered_map<std::string, std::unordered_map<std::int64_t, std::string>> dynamic_axes;
        auto exported =
            torch::jit::export_onnx(graph, {}, static_cast<std::int64_t>(opset_version), dynamic_axes, false,
                                    ::torch::onnx::OperatorExportTypes::ONNX, true, false, {}, true, false, staged_model.string());
        const auto& model_proto = std::get<0>(exported);
        if (model_proto == nullptr) { throw std::runtime_error("torch::jit::export_onnx returned a null ONNX model"); }
        write_onnx_model_bytes(torch::jit::serialize_model_proto_to_string(model_proto), staged_model, model.class_layout()->record());
        if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
        if (simplify) { simplify_onnx_model_file(staged_model); }
        const auto reopened = load_onnx_model_info(staged_model);
        if (!reopened.class_layout || *reopened.class_layout != model.class_layout()->record())
            throw std::runtime_error("ONNX export lost its class layout");
        publication.Publish({}, [&] { return stop.stop_requested(); });
        restore_attention();
        mmltk::common::logging::info([&](auto& logger) { logger.info("exported RF-DETR ONNX model to {}", output_path.string()); });
    } catch (...) {
        restore_attention();
        throw;
    }
}

void export_onnx(NativeRfDetrModel& model, const ExportOnnxRequest& request) {
    validate_export_onnx_request(request);
    export_model_onnx(model, request.output_path, request.opset_version, 1, request.simplify, request.class_layout_path, {});
}

void export_onnx(const ExportOnnxRequest& request) {
    ExportOnnxSession session;
    const auto stream = torch_cuda::current_torch_cuda_stream(request.device_id);
    session.Run(request, {.native_handle = stream, .valid = true});
}

struct ExportOnnxSession::State final {
    std::unique_ptr<NativeRfDetrModel> model;
    std::filesystem::path weights_path;
    std::optional<ClassArtifactSnapshot> admitted_snapshot;
    std::string preset_name;
    int resolution = 0;
    int device = -1;
    runtime::BorrowedCommandStream command_stream{};

    [[nodiscard]] cudaError_t Close() noexcept;
    void Run(const ExportOnnxRequest& request, runtime::BorrowedCommandStream execution_stream, std::stop_token stop);
};

ExportOnnxSession::ExportOnnxSession() : state_(std::make_unique<State>()) {}
ExportOnnxSession::~ExportOnnxSession() { static_cast<void>(Close()); }

ModelExportStatus ExportOnnxSession::Close() noexcept { return static_cast<ModelExportStatus>(state_->Close()); }

cudaError_t ExportOnnxSession::State::Close() noexcept {
    if (!model) return cudaSuccess;
    const auto selected = cudaSetDevice(device);
    if (selected != cudaSuccess) return selected;
    const auto settled = cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(command_stream.native_handle));
    if (settled != cudaSuccess) return settled;
    model.reset();
    return cudaSuccess;
}

void ExportOnnxSession::Run(const ExportOnnxRequest& request, const runtime::BorrowedCommandStream command_stream, const std::stop_token stop) {
    if (stop.stop_requested()) return;
    if (!command_stream) throw std::invalid_argument("RF-DETR ONNX export command stream is invalid");
    validate_export_onnx_request(request);
    struct BoundExportCall final {
        State* state;
        const ExportOnnxRequest* request;
        runtime::BorrowedCommandStream command_stream;
        std::stop_token stop;
    } call{
        .state = state_.get(),
        .request = &request,
        .command_stream = command_stream,
        .stop = stop,
    };
    try {
        torch_cuda::run_on_torch_cuda_stream(request.device_id, command_stream.native_handle, &call, [](void* opaque) {
            auto& bound = *static_cast<BoundExportCall*>(opaque);
            bound.state->Run(*bound.request, bound.command_stream, bound.stop);
        });
    } catch (const ArtifactPublicationCancelled&) {
        // The synchronous caller owns the stop token and its terminal outcome.
    }
}

void ExportOnnxSession::State::Run(const ExportOnnxRequest& request, const runtime::BorrowedCommandStream execution_stream, const std::stop_token stop) {
    if (stop.stop_requested()) return;
    const auto snapshot = ClassArtifactSnapshot::Read(request.weights_path, request.class_layout_path);
    if (!model || admitted_snapshot != snapshot || command_stream != execution_stream || weights_path != request.weights_path || preset_name != request.preset_name || resolution != request.resolution ||
        device != request.device_id) {
        if (model) {
            const auto status = Close();
            if (status != cudaSuccess) throw runtime::CudaOperationError{status, "RF-DETR export session rebind"};
        }
        auto resolved = resolve_model_state(request.weights_path, request.preset_name, request.resolution, request.class_layout_path);
        if (stop.stop_requested()) return;
        auto next_model = std::make_unique<NativeRfDetrModel>(resolved.artifacts.config, resolved.artifacts.class_layout);
        auto& technical_model = detail::native_model_owner(*next_model);
        static_cast<void>(technical_model.load_normalized_state(detail::model_state_owner(resolved.model_state).entries, false));
        technical_model.module().to(torch::Device(torch::kCUDA, static_cast<c10::DeviceIndex>(request.device_id)));
        if (snapshot != ClassArtifactSnapshot::Read(request.weights_path, request.class_layout_path))
            throw std::runtime_error("model artifact changed during export admission");
        admitted_snapshot = snapshot;
        model = std::move(next_model);
        weights_path = request.weights_path;
        preset_name = request.preset_name;
        resolution = request.resolution;
        device = request.device_id;
        command_stream = execution_stream;
    }
    export_model_onnx(*model, request.output_path, request.opset_version, 1, request.simplify, request.class_layout_path, stop);
}

}  // namespace mmltk::backend::models::rfdetr
