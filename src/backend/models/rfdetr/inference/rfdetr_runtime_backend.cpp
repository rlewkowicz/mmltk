module;
#include "prediction_capacity.h"
#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "detection_types.h"
#include "draw.h"
#include "postprocess.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/model_info.h"
#include "torch_api.h"

// CLEANUP-IGNORE: This runtime-backend unit imports the concrete artifact and tensor owners used by its boundary.
module mmltk.backend.models.rfdetr.inference.runtime_backend;

// CLEANUP-IGNORE: Runtime-backend and validation units import distinct concrete owners at their separate implementation
// boundaries.
import mmltk.backend.ml.cuda.torch_scope;
// CLEANUP-IGNORE: The runtime backend's artifact resolver starts a distinct import suffix from validation
// orchestration.
import mmltk.backend.models.rfdetr.core.artifact_resolution;

namespace mmltk::backend::models::rfdetr {
namespace runtime = mmltk::backend::ml::runtime;
namespace tensor_api = mmltk::backend::ml::torch_api;

namespace {

[[nodiscard]] std::string normalized_backend_name(std::string name) {
    std::ranges::transform(name, name.begin(), [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return name;
}

[[noreturn]] void throw_invalid_artifact_kind() { throw std::invalid_argument("RF-DETR inference artifact discriminator is invalid"); }

[[nodiscard]] tensor_api::ScalarType torch_type(runtime::RuntimeElementType type) {
    switch (type) {
        case runtime::RuntimeElementType::Float16:
            return tensor_api::kHalf;
        case runtime::RuntimeElementType::Float32:
            return tensor_api::kFloat;
        case runtime::RuntimeElementType::Int32:
            return tensor_api::kInt;
        case runtime::RuntimeElementType::Int64:
            return tensor_api::kLong;
        case runtime::RuntimeElementType::Bool:
            return tensor_api::kBool;
    }
    throw std::invalid_argument("unsupported RF-DETR runtime element type");
}

[[nodiscard]] std::size_t element_bytes(runtime::RuntimeElementType type) {
    switch (type) {
        case runtime::RuntimeElementType::Float16:
            return 2U;
        case runtime::RuntimeElementType::Float32:
        case runtime::RuntimeElementType::Int32:
            return 4U;
        case runtime::RuntimeElementType::Int64:
            return 8U;
        case runtime::RuntimeElementType::Bool:
            return 1U;
    }
    throw std::invalid_argument("unsupported RF-DETR runtime element type");
}

[[nodiscard]] std::string runtime_element_type_name(const runtime::RuntimeElementType type) {
    switch (type) {
        case runtime::RuntimeElementType::Float16:
            return "float16";
        case runtime::RuntimeElementType::Float32:
            return "float32";
        case runtime::RuntimeElementType::Int32:
            return "int32";
        case runtime::RuntimeElementType::Int64:
            return "int64";
        case runtime::RuntimeElementType::Bool:
            return "bool";
    }
    throw std::invalid_argument("unsupported RF-DETR runtime element type");
}

[[nodiscard]] TensorInfo project_runtime_tensor(const runtime::RuntimeTensorDescriptor& descriptor) {
    if (descriptor.shape.rank > descriptor.shape.extents.size()) { throw std::invalid_argument("RF-DETR runtime tensor rank is invalid"); }
    TensorInfo projected;
    projected.name = descriptor.name;
    projected.dtype = runtime_element_type_name(descriptor.element_type);
    projected.shape.assign(descriptor.shape.extents.begin(), descriptor.shape.extents.begin() + descriptor.shape.rank);
    return projected;
}

[[nodiscard]] ModelInfo project_runtime_model(const runtime::RuntimeModelInfo& runtime_info, const std::string_view backend_name) {
    if (runtime_info.output_count > runtime_info.outputs.size()) {
        throw std::invalid_argument("RF-DETR runtime output count exceeds fixed metadata capacity");
    }
    ModelInfo info;
    info.backend = backend_name;
    info.model_path = runtime_info.model_path.string();
    info.input = project_runtime_tensor(runtime_info.input);
    info.outputs.reserve(runtime_info.output_count);
    for (std::size_t index = 0U; index < runtime_info.output_count; ++index) {
        info.outputs.push_back(project_runtime_tensor(runtime_info.outputs[index]));
    }
    infer_rfdetr_output_layout(info);
    return info;
}

void close_runtime_backend(RfdetrRuntimeBackend& backend, const std::string_view operation) {
    const auto status = backend.Close();
    if (status != cudaSuccess) throw runtime::CudaOperationError{status, operation};
}

[[nodiscard]] std::size_t checked_element_count(const runtime::RuntimeShape& shape, std::size_t element_size) {
    std::size_t count = 1U;
    for (std::size_t index = 0U; index < shape.rank; ++index) {
        const auto extent = shape.extents[index];
        if (extent <= 0 || count > kMaximumPredictionTensorBytes / element_size / static_cast<std::size_t>(extent)) {
            throw std::invalid_argument("RF-DETR output has an invalid resolved shape");
        }
        count *= static_cast<std::size_t>(extent);
    }
    return count;
}

void validate_static_resolution(const runtime::RuntimeModelInfo& model, std::uint32_t requested) {
    if (model.input.shape.rank != 4U) { throw std::invalid_argument("RF-DETR input must use NCHW rank four"); }
    const auto height = model.input.shape.extents[2];
    const auto width = model.input.shape.extents[3];
    if (height <= 0 || width <= 0 || height != width) { throw std::invalid_argument("RF-DETR input must be statically square"); }
    if (requested != 0U && (height != static_cast<std::int64_t>(requested) || width != static_cast<std::int64_t>(requested))) {
        throw std::invalid_argument("RF-DETR artifact resolution does not match the request");
    }
}

void validate_annotation_buffer(const runtime::AnalysisDeviceBuffer& buffer, runtime::AnalysisElementType type, std::size_t bytes,
                                std::uint8_t rank, std::uint32_t leading_extent, std::uint32_t terminal_extent, const char* semantic) {
    if (buffer.address == 0U || buffer.element_type != type || buffer.capacity_bytes < bytes || buffer.shape.rank != rank ||
        buffer.shape.extents[0] < leading_extent || (rank == 2U && buffer.shape.extents[1] != terminal_extent)) {
        throw std::invalid_argument(std::string("RF-DETR ") + semantic + " storage is incompatible");
    }
}

}  // namespace

ResolvedInferenceArtifact resolve_inference_artifact(const ModelArtifactRequest& artifacts, std::string backend) {
    backend = normalized_backend_name(std::move(backend));
    if (backend.empty() || backend == "auto") {
        const auto selected_count = static_cast<unsigned int>(!artifacts.weights_path.empty()) +
                                    static_cast<unsigned int>(!artifacts.onnx_path.empty()) +
                                    static_cast<unsigned int>(!artifacts.tensorrt_path.empty());
        if (selected_count != 1U) { throw std::invalid_argument("automatic RF-DETR inference requires exactly one model artifact"); }
        if (!artifacts.tensorrt_path.empty()) {
            backend = "tensorrt";
        } else if (!artifacts.onnx_path.empty()) {
            backend = "onnx";
        } else if (!artifacts.weights_path.empty()) {
            backend = "weights";
        }
    }
    if (backend == "weights" || backend == "native") {
        if (artifacts.weights_path.empty()) { throw std::invalid_argument("RF-DETR weights backend requires a weights artifact"); }
        return {
            .kind = InferenceArtifactKind::Weights,
            .backend_name = "weights",
            .path = std::filesystem::absolute(artifacts.weights_path),
        };
    }
    if (backend == "onnx") {
        if (artifacts.onnx_path.empty()) { throw std::invalid_argument("RF-DETR ONNX backend requires an ONNX artifact"); }
        return {
            .kind = InferenceArtifactKind::Onnx,
            .backend_name = "onnx",
            .path = std::filesystem::absolute(artifacts.onnx_path),
        };
    }
    if (backend == "tensorrt" || backend == "trt") {
        if (!artifacts.tensorrt_path.empty()) {
            return {
                .kind = InferenceArtifactKind::TensorRt,
                .backend_name = "tensorrt",
                .path = std::filesystem::absolute(artifacts.tensorrt_path),
            };
        }
        if (!artifacts.onnx_path.empty()) {
            return {
                .kind = InferenceArtifactKind::TensorRt,
                .backend_name = "tensorrt",
                .path = std::filesystem::absolute(artifacts.onnx_path),
                .compile_onnx_to_tensorrt = true,
            };
        }
        throw std::invalid_argument("RF-DETR TensorRT backend requires TensorRT or ONNX input");
    }
    throw std::invalid_argument("unknown RF-DETR inference backend: " + backend);
}

ModelArtifactRequest select_inference_artifact(const ModelArtifactRequest& artifacts, const ResolvedInferenceArtifact& resolved) {
    ModelArtifactRequest selected;
    selected.preset_name = artifacts.preset_name;
    selected.resolution = artifacts.resolution;
    switch (resolved.kind) {
        case InferenceArtifactKind::Weights:
            selected.weights_path = resolved.path;
            break;
        case InferenceArtifactKind::Onnx:
            selected.onnx_path = resolved.path;
            break;
        case InferenceArtifactKind::TensorRt:
            if (resolved.compile_onnx_to_tensorrt) {
                selected.onnx_path = resolved.path;
            } else {
                selected.tensorrt_path = resolved.path;
            }
            break;
        default:
            throw_invalid_artifact_kind();
    }
    return selected;
}

ResolvedModelArtifacts describe_inference_artifact(const ModelArtifactRequest& request, const ResolvedInferenceArtifact& artifact,
                                                   const std::uint32_t resolution) {
    switch (artifact.kind) {
        case InferenceArtifactKind::Weights:
            return resolve_model_artifacts(artifact.path, request.preset_name, static_cast<int>(resolution));
        case InferenceArtifactKind::Onnx:
        case InferenceArtifactKind::TensorRt:
            break;
        default:
            throw_invalid_artifact_kind();
    }
    ResolvedModelArtifacts result;
    result.input_kind = artifact.backend_name;
    result.input_path = artifact.path;
    result.onnx_path = request.onnx_path;
    result.tensorrt_path = request.tensorrt_path;
    result.preset_name = request.preset_name;
    result.model_id = artifact.path.stem().string();
    result.artifact_root = artifact.path.parent_path();
    const PresetCatalogEntry* preset = nullptr;
    if (!request.preset_name.empty()) {
        preset = find_model_preset(request.preset_name);
        if (preset == nullptr) { throw std::invalid_argument("unknown RF-DETR preset override: " + request.preset_name); }
    } else {
        preset = infer_model_preset_from_path(artifact.path);
    }
    if (preset == nullptr) { throw std::invalid_argument("unable to infer RF-DETR preset from inference artifact"); }
    result.config = native_config_from_preset(*preset);
    result.config.resolution = static_cast<int>(resolution);
    result.automatic_num_queries_cap = result.config.num_queries;
    result.source_num_queries = result.config.num_queries;
    result.source_num_select = result.config.num_select;
    return result;
}

struct OutputStorage final {
    std::vector<tensor_api::Tensor> tensors;
    std::vector<runtime::RuntimeTensorBuffer> buffers;
};

struct RfdetrRuntimeBackend::State final {
    std::optional<std::size_t> masks;
    std::size_t logits = std::numeric_limits<std::size_t>::max();
    std::size_t boxes = std::numeric_limits<std::size_t>::max();
    std::shared_ptr<OutputStorage> outputs;
    std::vector<std::shared_ptr<PostprocessedSelection>> selections;
};

RfdetrRuntimeBackend::RfdetrRuntimeBackend(std::shared_ptr<runtime::RuntimeBackend> lane, std::string backend_name,
                                           std::uint32_t static_resolution, std::size_t maximum_detections)
    : lane_(std::move(lane)),
      backend_name_(std::move(backend_name)),
      static_resolution_(static_resolution),
      maximum_detections_(maximum_detections),
      state_(std::make_unique<State>()) {
    validate_prediction_candidates(maximum_detections_);
    if (!lane_) {
        throw std::invalid_argument("invalid RF-DETR runtime options");
    }
    const auto& model = lane_->model_info();
    for (std::size_t index = 0U; index < model.output_count; ++index) {
        const auto& output = model.outputs[index];
        if (output.shape.rank == 4U && normalized_backend_name(output.name).find("mask") != std::string::npos && !state_->masks) {
            state_->masks = index;
            continue;
        }
        if (output.shape.rank != 3U) { throw std::invalid_argument("RF-DETR standard outputs must be rank-three"); }
        const auto name = normalized_backend_name(output.name);
        if (name.find("box") != std::string::npos && output.shape.extents[2] == 4 &&
            state_->boxes == std::numeric_limits<std::size_t>::max()) {
            state_->boxes = index;
        } else if (name.find("logit") != std::string::npos && output.shape.extents[2] > 0 &&
                   state_->logits == std::numeric_limits<std::size_t>::max()) {
            state_->logits = index;
        } else {
            throw std::invalid_argument("RF-DETR output layout is not logits plus boxes");
        }
    }
    if (model.output_count != (state_->masks ? 3U : 2U) || state_->logits == std::numeric_limits<std::size_t>::max() ||
        state_->boxes == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("RF-DETR requires exactly logits and boxes outputs");
    }
    const auto& logits = model.outputs[state_->logits];
    const auto& boxes = model.outputs[state_->boxes];
    if ((logits.element_type != runtime::RuntimeElementType::Float16 && logits.element_type != runtime::RuntimeElementType::Float32) ||
        boxes.element_type != logits.element_type || logits.shape.extents[1] <= 0 || boxes.shape.extents[1] != logits.shape.extents[1] ||
        (boxes.shape.extents[0] > 0 && logits.shape.extents[0] > 0 && boxes.shape.extents[0] != logits.shape.extents[0])) {
        throw std::invalid_argument("RF-DETR logits and boxes have incompatible shapes or types");
    }
    if (state_->masks) {
        const auto& masks = model.outputs[*state_->masks];
        if (masks.element_type != logits.element_type || masks.shape.extents[1] != logits.shape.extents[1] ||
            masks.shape.extents[2] <= 0 || masks.shape.extents[3] <= 0 ||
            (masks.shape.extents[0] > 0 && logits.shape.extents[0] > 0 && masks.shape.extents[0] != logits.shape.extents[0]))
            throw std::invalid_argument("RF-DETR masks have incompatible shape or type");
    }
    state_->outputs = std::make_shared<OutputStorage>();
    state_->outputs->tensors.resize(model.output_count);
    state_->outputs->buffers.resize(model.output_count);
}

RfdetrRuntimeBackend::~RfdetrRuntimeBackend() = default;
RfdetrRuntimeBackend::RfdetrRuntimeBackend(RfdetrRuntimeBackend&&) noexcept = default;
RfdetrRuntimeBackend& RfdetrRuntimeBackend::operator=(RfdetrRuntimeBackend&&) noexcept = default;

const std::string& RfdetrRuntimeBackend::backend_name() const noexcept { return backend_name_; }

bool RfdetrRuntimeBackend::has_masks() const noexcept { return state_->masks.has_value(); }
const runtime::RuntimeShape& RfdetrRuntimeBackend::logits_shape() const noexcept { return model_info().outputs[state_->logits].shape; }
std::uint32_t RfdetrRuntimeBackend::static_resolution() const noexcept { return static_resolution_; }

std::int32_t RfdetrRuntimeBackend::device() const noexcept { return lane_->device(); }

std::uintptr_t RfdetrRuntimeBackend::stream() const noexcept { return lane_->command_stream().native_handle; }

runtime::RuntimeElementType RfdetrRuntimeBackend::input_element_type() const noexcept { return lane_->model_info().input.element_type; }

const runtime::RuntimeModelInfo& RfdetrRuntimeBackend::model_info() const noexcept { return lane_->model_info(); }

runtime::RuntimeSubmission RfdetrRuntimeBackend::Run(const runtime::RuntimeTensorBuffer& input,
                                                     std::span<runtime::AnalysisAnnotationStorage> annotations, std::span<RfdetrMaskSelection> selections, bool include_masks) {
    if (input.shape.rank != 4U || input.shape.extents[0] <= 0 || annotations.size() != static_cast<std::size_t>(input.shape.extents[0])) {
        throw std::invalid_argument("RF-DETR annotation count must equal the input batch");
    }
    if (!selections.empty() && selections.size() != annotations.size()) throw std::invalid_argument("RF-DETR selection count must equal the input batch");
    const std::int64_t batch = input.shape.extents[0];
    const auto output_storage = state_->outputs;
    if (!selections.empty()) state_->selections.resize(selections.size());

    for (auto& annotation : annotations) {
        const auto& logits = lane_->model_info().outputs[state_->logits];
        const auto capacity = PredictionCapacity::Resolve(std::min(maximum_detections_, annotation.value_capacity),
            static_cast<std::size_t>(batch), logits.shape.extents[1], logits.shape.extents[2], annotation.source_region.width,
            annotation.source_region.height, annotation.masks.address != 0U).candidates;
        if (!annotation.source_region.valid() || capacity == 0U) {
            throw std::invalid_argument("RF-DETR annotation region or capacity is invalid");
        }
        validate_annotation_buffer(annotation.boxes_xyxy, runtime::AnalysisElementType::Float32, capacity * 4U * sizeof(float), 2U,
                                   static_cast<std::uint32_t>(capacity), 4U, "box");
        validate_annotation_buffer(annotation.category_ids, runtime::AnalysisElementType::Int32, capacity * sizeof(std::int32_t), 1U,
                                   static_cast<std::uint32_t>(capacity), 0U, "category");
        validate_annotation_buffer(annotation.confidences, runtime::AnalysisElementType::Float32, capacity * sizeof(float), 1U,
                                   static_cast<std::uint32_t>(capacity), 0U, "confidence");
        annotation.value_count = 0U;
    }

    struct Context final {
        RfdetrRuntimeBackend* owner;
        OutputStorage* outputs;
        std::int64_t batch;
        std::span<runtime::AnalysisAnnotationStorage> annotations;
        std::span<RfdetrMaskSelection> selections;
        bool include_masks;
    } context{this, output_storage.get(), batch, annotations, selections, include_masks};

    const runtime::RuntimeOutputBinding binding{
        .context = &context,
        .bind =
            [](void* opaque, std::int32_t device, std::uintptr_t stream_value, std::span<runtime::RuntimeTensorBuffer> buffers) {
                auto& call = *static_cast<Context*>(opaque);
                auto& owner = *call.owner;
                if (device != owner.lane_->device() || stream_value != owner.lane_->command_stream().native_handle ||
                    buffers.size() != owner.model_info().output_count) {
                    throw std::runtime_error("RF-DETR output binding received the wrong lane");
                }
                const auto options = tensor_api::TensorOptions().device(tensor_api::kCUDA, device);
                for (std::size_t index = 0U; index < buffers.size(); ++index) {
                    runtime::RuntimeShape shape = owner.model_info().outputs[index].shape;
                    shape.extents[0] = call.batch;
                    const auto type = owner.model_info().outputs[index].element_type;
                    const auto count = checked_element_count(shape, element_bytes(type));
                    const tensor_api::IntArrayRef extents(shape.extents.data(), shape.rank);
                    auto& tensor = call.outputs->tensors[index];
                    if (!tensor.defined()) {
                        tensor = tensor_api::empty(extents, options.dtype(torch_type(type)));
                    } else {
                        tensor.resize_(extents);
                    }
                    buffers[index] = {
                        .device_data = tensor.data_ptr(),
                        .capacity_bytes = count * element_bytes(type),
                        .shape = shape,
                        .element_type = type,
                    };
                }
            },
    };

    const runtime::RuntimeContinuation continuation{
        .context = &context,
        .enqueue =
            [](void* opaque, std::int32_t device, std::uintptr_t stream_value) {
                auto& call = *static_cast<Context*>(opaque);
                auto& owner = *call.owner;
                if (device != owner.lane_->device() || stream_value != owner.lane_->command_stream().native_handle) {
                    throw std::runtime_error("RF-DETR continuation received the wrong lane");
                }
                mmltk::backend::ml::cuda::run_on_torch_cuda_stream(device, stream_value, opaque, [](void* bound_context) {
                    auto& bound_call = *static_cast<Context*>(bound_context);
                    auto& bound_owner = *bound_call.owner;
                    const auto bound_device = bound_owner.lane_->device();
                    const auto bound_stream = bound_owner.lane_->command_stream().native_handle;
                    auto logits = bound_call.outputs->tensors[bound_owner.state_->logits];
                    auto all_boxes = bound_call.outputs->tensors[bound_owner.state_->boxes];

                    for (std::size_t index = 0U; index < bound_call.annotations.size(); ++index) {
                        auto& annotation = bound_call.annotations[index];
                        const auto limit = std::min({bound_owner.maximum_detections_, annotation.value_capacity,
                                                     static_cast<std::size_t>(logits.size(1) * logits.size(2))});
                        const auto& region = annotation.source_region;
                        auto selected = select_output_batch_fixed_size(
                            OutputTensors{
                                .pred_logits = logits.narrow(0, static_cast<std::int64_t>(index), 1),
                                .pred_boxes = all_boxes.narrow(0, static_cast<std::int64_t>(index), 1),
                                .pred_masks = bound_call.include_masks && bound_owner.state_->masks ? std::optional{bound_call.outputs->tensors[*bound_owner.state_->masks].narrow(0, static_cast<std::int64_t>(index), 1)} : std::nullopt,
                            },
                            region.height, region.width, static_cast<std::int64_t>(limit));
                        PostprocessedBatch processed{selected.scores, selected.labels, selected.boxes, std::nullopt};
                        if (!bound_call.selections.empty()) {
                            auto& custody = bound_owner.state_->selections[index];
                            if (!custody) custody = std::make_shared<PostprocessedSelection>();
                            *custody = selected;
                            auto& destination = bound_call.selections[index];
                            destination = {.query_indices = {
                                .device_data = selected.query_indices.data_ptr(),
                                .capacity_bytes = static_cast<std::size_t>(selected.query_indices.numel() * selected.query_indices.element_size()),
                                .shape = {.rank = 2U, .extents = {1, static_cast<std::int64_t>(limit)}},
                                .element_type = runtime::RuntimeElementType::Int64}, .custody = custody};
                            if (selected.mask_logits) {
                                auto mask_buffer = bound_call.outputs->buffers[*bound_owner.state_->masks];
                                mask_buffer.device_data = selected.mask_logits->data_ptr();
                                mask_buffer.shape.extents[0] = 1;
                                mask_buffer.capacity_bytes = selected.mask_logits->numel() * selected.mask_logits->element_size();
                                destination.mask_logits = mask_buffer;
                            }
                        } else if (selected.mask_logits) processed.masks = materialize_selected_masks(*selected.mask_logits, selected.query_indices, region.height, region.width);
                        auto scores = processed.scores[0];
                        auto labels = processed.labels[0].to(tensor_api::kInt);
                        auto xyxy = processed.boxes[0].to(tensor_api::kFloat);
                        const auto count = static_cast<std::size_t>(scores.size(0));

                        const auto cuda_options = tensor_api::TensorOptions().device(tensor_api::kCUDA, bound_device);
                        const std::array<std::int64_t, 2> boxes_shape{static_cast<std::int64_t>(count), 4};
                        const std::array<std::int64_t, 1> values_shape{static_cast<std::int64_t>(count)};
                        tensor_api::from_blob(reinterpret_cast<void*>(annotation.boxes_xyxy.address), tensor_api::IntArrayRef{boxes_shape},
                                              cuda_options.dtype(tensor_api::kFloat))
                            .copy_(xyxy);
                        tensor_api::from_blob(reinterpret_cast<void*>(annotation.category_ids.address),
                                              tensor_api::IntArrayRef{values_shape}, cuda_options.dtype(tensor_api::kInt))
                            .copy_(labels);
                        tensor_api::from_blob(reinterpret_cast<void*>(annotation.confidences.address),
                                              tensor_api::IntArrayRef{values_shape}, cuda_options.dtype(tensor_api::kFloat))
                            .copy_(scores.to(tensor_api::kFloat));
                        if (processed.masks && annotation.masks.address != 0U) {
                            const auto bytes = count * region.width * region.height;
                            validate_annotation_buffer(annotation.masks, runtime::AnalysisElementType::Uint8, bytes, 3U,
                                                       static_cast<std::uint32_t>(annotation.value_capacity), region.height, "mask");
                            if (annotation.masks.shape.extents[1] != region.height || annotation.masks.shape.extents[2] != region.width)
                                throw std::invalid_argument("RF-DETR mask storage has wrong geometry");
                            const std::array<std::int64_t, 3> mask_shape{static_cast<std::int64_t>(count), region.height, region.width};
                            tensor_api::from_blob(reinterpret_cast<void*>(annotation.masks.address), tensor_api::IntArrayRef{mask_shape},
                                                  cuda_options.dtype(tensor_api::kUInt8)).copy_((*processed.masks)[0]);
                        }
                        if (annotation.colors_rgb.address != 0U) {
                            build_instance_colors_async(reinterpret_cast<const std::int32_t*>(annotation.category_ids.address), count,
                                                        static_cast<int>(logits.size(2)),
                                                        reinterpret_cast<std::uint8_t*>(annotation.colors_rgb.address),
                                                        reinterpret_cast<cudaStream_t>(bound_stream));
                        }
                    }
                    for (auto& annotation : bound_call.annotations) {
                        annotation.value_count = std::min({bound_owner.maximum_detections_, annotation.value_capacity,
                                                           static_cast<std::size_t>(logits.size(1) * logits.size(2))});
                    }
                });
            },
    };
    try {
        std::shared_ptr<void> retained_storage = output_storage;
        return lane_->Run(input, output_storage->buffers, binding, continuation, std::move(retained_storage));
    } catch (...) {
        for (auto& annotation : annotations) {
            annotation.value_count = 0U;
        }
        throw;
    }
}

void RfdetrRuntimeBackend::ReleaseAfterCompletion(runtime::RuntimeSubmission&& submission) {
    lane_->ReleaseAfterCompletion(std::move(submission));
}

runtime::RuntimeStatus RfdetrRuntimeBackend::Close() noexcept { return lane_->Close(); }

std::shared_ptr<RfdetrRuntimeBackend> RfdetrRuntimeBackend::MakeLane() const {
    return std::shared_ptr<RfdetrRuntimeBackend>(
        new RfdetrRuntimeBackend(lane_->MakeLane(), backend_name_, static_resolution_, maximum_detections_));
}

std::shared_ptr<RfdetrRuntimeBackend> make_rfdetr_runtime_backend(const RfdetrRuntimeBackendOptions& options) {
    if (!options.command_stream) { throw std::invalid_argument("RF-DETR runtime command stream is invalid"); }
    const auto artifact = resolve_inference_artifact(options.artifacts, options.backend);
    runtime::RuntimeBackendKind kind;
    switch (artifact.kind) {
        case InferenceArtifactKind::Weights:
            throw std::invalid_argument("native RF-DETR weights use the core model execution path");
        case InferenceArtifactKind::Onnx:
            kind = runtime::RuntimeBackendKind::Onnx;
            break;
        case InferenceArtifactKind::TensorRt:
            kind = runtime::RuntimeBackendKind::TensorRt;
            break;
        default:
            throw_invalid_artifact_kind();
    }
    auto lane = runtime::make_runtime_backend({.kind = kind,
                                               .model_path = artifact.path,
                                               .save_compiled_model_path = options.save_compiled_model_path,
                                               .device = options.device,
                                               .command_stream = options.command_stream,
                                               .allow_fp16 = options.allow_fp16});
    validate_static_resolution(lane->model_info(), options.static_resolution);
    const auto resolution = static_cast<std::uint32_t>(lane->model_info().input.shape.extents[2]);
    return std::shared_ptr<RfdetrRuntimeBackend>(
        new RfdetrRuntimeBackend(std::move(lane), artifact.backend_name, resolution, options.maximum_detections));
}

std::vector<std::shared_ptr<RfdetrRuntimeBackend>> make_rfdetr_runtime_backend_lanes(const RfdetrRuntimeBackendOptions& options,
                                                                                     std::size_t lane_count) {
    if (lane_count == 0U) { throw std::invalid_argument("RF-DETR runtime lane count must be positive"); }
    auto first = make_rfdetr_runtime_backend(options);
    std::vector<std::shared_ptr<RfdetrRuntimeBackend>> lanes;
    lanes.reserve(lane_count);
    lanes.push_back(first);
    for (std::size_t index = 1U; index < lane_count; ++index) {
        lanes.push_back(first->MakeLane());
    }
    return lanes;
}

ModelInfo inspect_tensorrt_model(const ModelArtifactRequest& artifacts, const int device_id) {
    if (artifacts.selected_input_count() != 1U || artifacts.tensorrt_path.empty() || device_id < 0) {
        throw std::invalid_argument(
            "RF-DETR TensorRT inspection requires one engine and a valid "
            "device");
    }
    const auto stream = mmltk::backend::ml::cuda::current_torch_cuda_stream(device_id);
    auto backend = make_rfdetr_runtime_backend({
        .artifacts = artifacts,
        .backend = "tensorrt",
        .device = device_id,
        .command_stream =
            {
                .native_handle = stream,
                .valid = true,
            },
        .static_resolution = 0U,
        .maximum_detections = 500U,
        .save_compiled_model_path = {},
        .allow_fp16 = true,
    });
    ModelInfo info;
    try {
        info = project_runtime_model(backend->model_info(), backend->backend_name());
    } catch (...) {
        close_runtime_backend(*backend, "RF-DETR TensorRT inspection close");
        throw;
    }
    close_runtime_backend(*backend, "RF-DETR TensorRT inspection close");
    return info;
}

void build_tensorrt_engine(const BuildEngineRequest& request) {
    validate_build_engine_request(request);
    if (request.device_id < 0) { throw std::invalid_argument("RF-DETR TensorRT build device must be nonnegative"); }
    const auto stream = mmltk::backend::ml::cuda::current_torch_cuda_stream(request.device_id);
    build_tensorrt_engine(request, {.native_handle = stream, .valid = true});
}

void build_tensorrt_engine(const BuildEngineRequest& request, const runtime::BorrowedCommandStream command_stream) {
    validate_build_engine_request(request);
    auto backend = make_rfdetr_runtime_backend({
        .artifacts = request,
        .backend = "tensorrt",
        .device = request.device_id,
        .command_stream = command_stream,
        .static_resolution = request.resolution > 0 ? static_cast<std::uint32_t>(request.resolution) : 0U,
        .maximum_detections = 500U,
        .save_compiled_model_path = std::filesystem::absolute(request.output_path),
        .allow_fp16 = request.allow_fp16,
    });
    close_runtime_backend(*backend, "RF-DETR TensorRT engine close");
}

}  // namespace mmltk::backend::models::rfdetr
