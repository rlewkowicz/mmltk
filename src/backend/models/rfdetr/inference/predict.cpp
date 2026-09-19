module;
#include <c10/cuda/CUDAGuard.h>
#include <ATen/cuda/CUDAContext.h>
#include "src/backend/models/rfdetr/core/evaluator.h"
#include "src/backend/models/rfdetr/core/class_artifact.h"
#include "src/backend/models/rfdetr/core/detail/class_artifact_files.h"
#include "src/frameworks/gpu/cuda_context_scope.h"
#include "src/backend/ml/cuda/numa_host_tensor.h"
#include <cuda_runtime.h>
#include <c10/cuda/CUDAStream.h>
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "dataset_batch_lease.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/core/sample_output.h"
#include "src/backend/models/rfdetr/core/execution_precision.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/core/postprocess.h"
#include "src/backend/ml/torch/scalar_type.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include "src/backend/models/rfdetr/core/model_info.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "stb_image.h"
#include "src/backend/models/rfdetr/core/gpu_batch_preprocessor.h"
#include "prediction_capacity.h"
#include "prediction_count.h"
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
#include "prediction_raw_preparation.h"
#include "src/backend/media/video/video_file_source.h"
#include <ATen/ops/upsample_bilinear2d.h>
#include <ATen/ops/index_select.h>
#include <torch/types.h>
#include <torch/serialize.h>
#include "src/backend/models/rfdetr/core/runtime.h"
module mmltk.backend.models.rfdetr.inference.prediction;
// CLEANUP-IGNORE: This inference implementation imports the concrete owners used by its typed prediction boundary.
import mmltk.backend.ml.cuda.torch_scope;
import mmltk.backend.models.rfdetr.core.dataset_limit_resolution;
import mmltk.backend.models.rfdetr.inference.loader;
namespace mmltk::backend::models::rfdetr {
namespace runtime = mmltk::backend::ml::runtime;
namespace torch_cuda = mmltk::backend::ml::cuda;
namespace {
void finish_prediction_run(PredictionRunResult& result, const std::chrono::steady_clock::time_point started) noexcept {
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    result.timing = {
        .seconds = seconds,
        .img_per_s = seconds > 0.0 ? static_cast<double>(result.processed_images) / seconds : 0.0,
        .images = result.processed_images,
    };
}
[[nodiscard]] const char* predict_source_kind_name(const PredictSourceKind kind) {
    switch (kind) {
        case PredictSourceKind::CompiledDataset: return "compiled_dataset";
        case PredictSourceKind::ImageFiles: return "image_files";
        case PredictSourceKind::VideoFile: return "video_file";
    }
    throw std::invalid_argument("invalid RF-DETR prediction source kind");
}
[[nodiscard]] std::string encode_mask_rle(const EncodedMask& mask) {
    std::string encoded;
    if (mask.runs.size() > kMaximumPredictionMaskRuns) throw std::invalid_argument("RF-DETR RLE text exceeds supported capacity");
    encoded.reserve(mask.runs.size() * 22U);
    for (std::size_t index = 0U; index < mask.runs.size(); ++index) {
        if (index != 0U) { encoded.push_back(' '); }
        encoded += std::to_string(mask.runs[index].first);
        encoded.push_back(':');
        encoded += std::to_string(mask.runs[index].second);
    }
    return encoded;
}
[[nodiscard]] nlohmann::json prediction_record_json(const PredictionRecord& record, std::span<const std::string> class_names) {
    nlohmann::json detections = nlohmann::json::array();
    for (const Prediction& prediction : record.detections) {
        const int class_index = prediction.class_reference;
        std::string label = std::to_string(prediction.class_reference);
        if (prediction.class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::Foreground && class_index >= 0 &&
            static_cast<std::size_t>(class_index) < class_names.size()) {
            label = class_names[static_cast<std::size_t>(class_index)];
        }
        nlohmann::json detection = {
            {"label", std::move(label)},
            {"score", prediction.score},
            {"xyxy", prediction.bbox_xyxy},
        };
        if (prediction.has_mask) { detection["mask_rle"] = encode_mask_rle(prediction.mask); }
        detections.push_back(std::move(detection));
    }
    nlohmann::json payload = {
        {"dataset_index", record.dataset_index},
        {"image_id", record.image_id},
        {"detections", std::move(detections)},
    };
    if (!record.source_name.empty()) { payload["source_name"] = record.source_name; }
    return payload;
}
[[nodiscard]] runtime::RuntimeElementType runtime_type(const at::ScalarType type) {
    if (type == at::kFloat) { return runtime::RuntimeElementType::Float32; }
    if (type == at::kHalf) { return runtime::RuntimeElementType::Float16; }
    throw std::invalid_argument("RF-DETR input must be float16 or float32");
}
[[nodiscard]] runtime::RuntimeTensorBuffer input_buffer(const torch::Tensor& input) {
    runtime::RuntimeShape shape{.rank = static_cast<std::uint8_t>(input.dim())};
    if (input.dim() > static_cast<std::int64_t>(runtime::kMaximumRuntimeRank)) { throw std::invalid_argument("RF-DETR input rank exceeds runtime bound"); }
    for (std::int64_t index = 0; index < input.dim(); ++index) { shape.extents[static_cast<std::size_t>(index)] = input.size(index); }
    return {
        .device_data = input.data_ptr(),
        .capacity_bytes = static_cast<std::size_t>(input.numel() * input.element_size()),
        .shape = shape,
        .element_type = runtime_type(input.scalar_type()),
    };
}
struct AnnotationBatch final {
    torch::Tensor boxes;
    torch::Tensor labels;
    torch::Tensor scores;
    torch::Tensor masks;
    torch::Tensor compact_boxes, compact_labels, compact_scores;
    std::vector<runtime::AnalysisAnnotationStorage> storage;
    std::vector<PostprocessedSelection> selections;
    std::vector<RfdetrMaskSelection> runtime_selections;
    bool want_masks = false;
    bool encoded_masks = false;
    bool preview_masks = false;
    std::vector<PredictionDemand> demand;
    bool raw_preview = false;
    std::string preview_failure;
};
class PredictionBackend final {
   public:
    PredictionBackend(const PredictRequest& options, ResolvedInferenceArtifact artifact, const runtime::BorrowedCommandStream command_stream,
                      std::stop_token stop)
        : artifact_(std::move(artifact)), maximum_detections_(options.max_dets_per_image), device_(options.device_id), command_stream_(command_stream) {
        if (maximum_detections_ != 0) validate_prediction_candidates(maximum_detections_);
        switch (artifact_.kind) {
            case InferenceArtifactKind::Weights: {
                auto resolved =
                    resolve_model_state(artifact_.path, options.preset_name, options.resolution, options.class_layout_path, artifact_.admission, stop);
                artifact_.admission = resolved.model_state.class_artifact;
                artifacts_ = std::move(resolved.artifacts);
                if (maximum_detections_ == 0) maximum_detections_ = artifacts_.config.num_select;
                static_cast<void>(PredictionCapacity::Resolve(maximum_detections_, 1U, artifacts_.config.num_queries, artifacts_.config.num_classes,
                                                              artifacts_.config.resolution, artifacts_.config.resolution, false));
                native_ = std::make_unique<NativeRfDetrModel>(artifacts_.config, artifacts_.class_layout);
                class_postprocess_ = std::make_unique<ClassPostprocessLane>(native_->class_layout());
                class_postprocess_->Prepare(torch::Device(torch::kCUDA, static_cast<c10::DeviceIndex>(device_)));
                auto& technical_model = (*native_);
                static_cast<void>(technical_model.load_normalized_state(resolved.model_state.entries(), false));
                technical_model.to(torch::Device(torch::kCUDA, static_cast<c10::DeviceIndex>(device_)));
                technical_model.eval();
                native_precision_ = options.allow_fp16 ? torch_cuda::preferred_torch_cuda_precision(device_) : torch_cuda::TorchCudaPrecision::Float32;
                switch (native_precision_) {
                    case torch_cuda::TorchCudaPrecision::Float32: native_input_type_ = at::kFloat; break;
                    case torch_cuda::TorchCudaPrecision::Float16: native_input_type_ = at::kHalf; break;
                    case torch_cuda::TorchCudaPrecision::BFloat16: native_input_type_ = torch::kBFloat16; break;
                }
                native_autocast_enabled_ = options.allow_fp16;
                resolution_ = static_cast<std::uint32_t>(artifacts_.config.resolution);
                return;
            }
            case InferenceArtifactKind::Onnx:
            case InferenceArtifactKind::TensorRt: break;
            default: throw std::invalid_argument("invalid RF-DETR inference artifact kind");
        }
        artifacts_ = describe_inference_artifact(options, artifact_, options.resolution);
        if (maximum_detections_ == 0) maximum_detections_ = artifacts_.config.num_select;
        runtime_ = make_rfdetr_runtime_backend({
            .artifacts = options,
            .backend = artifact_.backend_name,
            .device = options.device_id,
            .command_stream = command_stream,
            .static_resolution = options.resolution > 0 ? static_cast<std::uint32_t>(options.resolution) : 0U,
            .maximum_detections = maximum_detections_,
            .save_compiled_model_path = {},
            .allow_fp16 = options.allow_fp16,
            .admission = artifact_.admission,
            .stop = stop,
        });
        artifact_.admission = runtime_->class_artifact();
        resolution_ = runtime_->static_resolution();
        artifacts_.config.resolution = static_cast<int>(resolution_);
        artifacts_.class_layout = runtime_->class_layout()->record();
        artifacts_.artifact_sha256 = runtime_->artifact_sha256();
        const auto& shape = runtime_->logits_shape();
        static_cast<void>(PredictionCapacity::Resolve(maximum_detections_, 1U, shape.extents[1], shape.extents[2], resolution_, resolution_, false));
        artifacts_.config.num_queries = static_cast<int>(shape.extents[1]);
        artifacts_.config.num_classes = static_cast<int>(shape.extents[2]);
        artifacts_.source_num_queries = artifacts_.config.num_queries;
    }
    ~PredictionBackend() { static_cast<void>(Close()); }
    [[nodiscard]] const std::shared_ptr<const ClassArtifactAdmission>& class_artifact() const noexcept { return artifact_.admission; }
    [[nodiscard]] const std::string& name() const noexcept { return artifact_.backend_name; }
    [[nodiscard]] std::uint32_t resolution() const noexcept { return resolution_; }
    [[nodiscard]] at::ScalarType input_type() const noexcept {
        if (runtime_ && runtime_->model_info().input.element_type == runtime::RuntimeElementType::Float16) { return at::kHalf; }
        return native_ ? native_input_type_ : at::kFloat;
    }
    [[nodiscard]] bool has_masks() const noexcept { return native_ ? artifacts_.config.segmentation : runtime_->has_masks(); }
    [[nodiscard]] std::size_t capacity(std::size_t batch, std::uint32_t width, std::uint32_t height, bool masks) const {
        return PredictionCapacity::Resolve(maximum_detections_, batch, artifacts_.config.num_queries, artifacts_.config.num_classes, width, height, masks,
                                           class_layout()->eligible_count())
            .candidates;
    }
    [[nodiscard]] std::size_t candidate_count() const noexcept { return maximum_detections_; }
    [[nodiscard]] const ResolvedModelArtifacts& artifacts() const noexcept { return artifacts_; }
    [[nodiscard]] const std::shared_ptr<const ResolvedClassLayout>& class_layout() const noexcept {
        return native_ ? native_->class_layout() : runtime_->class_layout();
    }
    void Execute(const torch::Tensor& input, AnnotationBatch& annotations) {
        if (runtime_) {
            auto submission = runtime_->Run(input_buffer(input), annotations.storage, annotations.runtime_selections, annotations.want_masks);
            runtime_->ReleaseAfterCompletion(std::move(submission));
            for (std::size_t index = 0; index < annotations.runtime_selections.size(); ++index) {
                const auto& selected = annotations.runtime_selections[index];
                if (selected.query_indices.device_data == nullptr) {
                    annotations.selections[index] = {};
                    continue;
                }
                const auto view = [&](const runtime::RuntimeTensorBuffer& buffer, at::ScalarType type) {
                    return torch::from_blob(buffer.device_data, at::IntArrayRef(buffer.shape.extents.data(), buffer.shape.rank), input.options().dtype(type));
                };
                auto& destination = annotations.selections[index];
                destination = {.query_indices = view(selected.query_indices, at::kLong)};
                if (annotations.want_masks && selected.mask_logits)
                    destination.mask_logits =
                        view(*selected.mask_logits, selected.mask_logits->element_type == runtime::RuntimeElementType::Float16 ? at::kHalf : at::kFloat);
            }
            return;
        }
        struct NativeCall final {
            PredictionBackend* owner;
            const torch::Tensor* input;
            AnnotationBatch* annotations;
        } call{this, &input, &annotations};
        torch_cuda::run_with_torch_cuda_scope({.device = device_,
                                               .stream = command_stream_.native_handle,
                                               .inference_mode = true,
                                               .autocast = native_autocast_enabled_,
                                               .precision = native_precision_},
                                              &call, [](void* opaque) {
                                                  auto& native_call = *static_cast<NativeCall*>(opaque);
                                                  native_call.owner->ExecuteNative(*native_call.input, *native_call.annotations);
                                              });
    }
    [[nodiscard]] runtime::RuntimeStatus Close() noexcept {
        if (runtime_) {
            const auto status = runtime_->Close();
            if (status != runtime::kRuntimeSuccess) return status;
            runtime_.reset();
        }
        if (native_) {
            const auto selected = cudaSetDevice(device_);
            if (selected != cudaSuccess) return static_cast<runtime::RuntimeStatus>(selected);
            const auto settled = cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(command_stream_.native_handle));
            if (settled != cudaSuccess) return static_cast<runtime::RuntimeStatus>(settled);
            class_postprocess_.reset();
            native_.reset();
        }
        return runtime::kRuntimeSuccess;
    }

   private:
    void ExecuteNative(const torch::Tensor& input, AnnotationBatch& annotations) {
        auto mask = torch::zeros({input.size(0), input.size(2), input.size(3)}, input.options().dtype(torch::kBool));
        const ModelOutputs outputs = (*native_).forward(NestedTensor{input, std::move(mask)}, annotations.want_masks);
        assert_inference_output_dtype(outputs.main.pred_logits, outputs.main.pred_boxes, native_input_type_, "native RF-DETR inference");
        count_storage_.resize(annotations.storage.size());
        for (std::size_t index = 0U; index < annotations.storage.size(); ++index) {
            auto& storage = annotations.storage[index];
            const auto count = std::min({maximum_detections_, storage.value_capacity,
                                         static_cast<std::size_t>(outputs.main.pred_logits.size(1) * outputs.main.pred_logits.size(2))});
            storage.class_catalog = native_->class_layout()->catalog();
            storage.class_domain = native_->class_layout()->domain();
            storage.masks_available = false;
            if (count == 0) {
                storage.value_count = 0;
                annotations.selections[index] = {};
                continue;
            }
            const auto processed = select_output_batch_fixed_size(
                OutputTensors{
                    .pred_logits = outputs.main.pred_logits.narrow(0, static_cast<std::int64_t>(index), 1),
                    .pred_boxes = outputs.main.pred_boxes.narrow(0, static_cast<std::int64_t>(index), 1),
                    .pred_masks = annotations.want_masks && outputs.main.pred_masks
                                      ? std::optional{outputs.main.pred_masks->narrow(0, static_cast<std::int64_t>(index), 1)}
                                      : std::nullopt,
                },
                storage.source_region.height, storage.source_region.width, static_cast<std::int64_t>(count), annotations.want_masks, class_postprocess_.get());
            const auto active = processed.scores.size(1);
            annotations.boxes[index].narrow(0, 0, active).copy_(processed.boxes[0].to(at::kFloat));
            annotations.labels[index].narrow(0, 0, active).copy_(processed.labels[0].to(at::kInt));
            annotations.scores[index].narrow(0, 0, active).copy_(processed.scores[0].to(at::kFloat));
            annotations.selections[index] = processed;
            publish_prediction_count(count_storage_[index], processed.counts, storage, device_);
            storage.class_catalog = native_->class_layout()->catalog();
            storage.class_domain = native_->class_layout()->domain();
        }
    }
    ResolvedInferenceArtifact artifact_;
    ResolvedModelArtifacts artifacts_;
    std::shared_ptr<RfdetrRuntimeBackend> runtime_;
    std::unique_ptr<NativeRfDetrModel> native_;
    std::unique_ptr<ClassPostprocessLane> class_postprocess_;
    std::vector<std::shared_ptr<PredictionCountStorage>> count_storage_;
    std::size_t maximum_detections_ = 0U;
    std::uint32_t resolution_ = 0U;
    int device_ = 0;
    runtime::BorrowedCommandStream command_stream_{};
    at::ScalarType native_input_type_ = at::kFloat;
    torch_cuda::TorchCudaPrecision native_precision_ = torch_cuda::TorchCudaPrecision::Float32;
    bool native_autocast_enabled_ = false;
};
void prepare_annotations(AnnotationBatch& result, std::size_t batch, std::size_t capacity, std::uint32_t width, std::uint32_t height, int device, bool masks,
                         bool raw_preview) {
    const auto cuda = torch::TensorOptions().device(torch::kCUDA, static_cast<c10::DeviceIndex>(device));
    const auto resize = [&](torch::Tensor& tensor, at::IntArrayRef shape, at::ScalarType type) {
        if (!tensor.defined() || tensor.get_device() != device)
            tensor = torch::empty(shape, cuda.dtype(type));
        else
            tensor.resize_(shape);
    };
    const auto count = static_cast<std::int64_t>(batch);
    const auto limit = static_cast<std::int64_t>(capacity);
    resize(result.boxes, {count, limit, 4}, at::kFloat);
    resize(result.labels, {count, limit}, at::kInt);
    resize(result.scores, {count, limit}, at::kFloat);
    result.want_masks = masks;
    result.raw_preview = raw_preview;
    result.preview_failure.clear();
    result.selections.resize(batch);
    result.runtime_selections.resize(batch);
    result.storage.resize(batch);
    for (std::size_t index = 0U; index < batch; ++index) {
        result.runtime_selections[index] = {};
        result.storage[index] = {
            .source_region = {.width = width, .height = height},
            .value_capacity = capacity,
            .boxes_xyxy =
                {
                    .address = reinterpret_cast<std::uintptr_t>(result.boxes[index].data_ptr()),
                    .capacity_bytes = capacity * 4U * sizeof(float),
                    .shape = {.rank = 2U, .extents = {static_cast<std::uint32_t>(capacity), 4U}},
                    .element_type = runtime::AnalysisElementType::Float32,
                },
            .class_references =
                {
                    .address = reinterpret_cast<std::uintptr_t>(result.labels[index].data_ptr()),
                    .capacity_bytes = capacity * sizeof(std::int32_t),
                    .shape = {.rank = 1U, .extents = {static_cast<std::uint32_t>(capacity)}},
                    .element_type = runtime::AnalysisElementType::Int32,
                },
            .confidences =
                {
                    .address = reinterpret_cast<std::uintptr_t>(result.scores[index].data_ptr()),
                    .capacity_bytes = capacity * sizeof(float),
                    .shape = {.rank = 1U, .extents = {static_cast<std::uint32_t>(capacity)}},
                    .element_type = runtime::AnalysisElementType::Float32,
                },
        };
    }
}
struct PredictionReadback final {
    explicit PredictionReadback(int device) : boxes(device), labels(device), scores(device), masks(device), indices(device) {}
    void Read(AnnotationBatch& batch) {
        box_values = boxes.view(batch.boxes.sizes(), at::kFloat);
        label_values = labels.view(batch.labels.sizes(), at::kInt);
        score_values = scores.view(batch.scores.sizes(), at::kFloat);
        box_values.copy_(batch.boxes, true);
        label_values.copy_(batch.labels, true);
        score_values.copy_(batch.scores, true);
        const auto stream = c10::cuda::getCurrentCUDAStream(batch.boxes.get_device());
        const auto status = cudaStreamSynchronize(stream.stream());
        if (status != cudaSuccess) throw runtime::CudaOperationError{status, "prediction readback completion"};
        for (auto& annotation : batch.storage) annotation.SettleValueCount();
        index_values = indices.view(batch.labels.sizes(), at::kLong);
    }
    mmltk::backend::ml::cuda::NumaHostTensor boxes, labels, scores, masks, indices;
    torch::Tensor device_indices, selected_queries;
    SelectedMaskWorkspace mask_workspace;
    torch::Tensor box_values, label_values, score_values, mask_values, index_values;
};
void execute_prediction_batch(const PredictRequest& options, PredictionBackend& backend, const torch::Tensor& input, std::size_t batch_size,
                              std::uint32_t width, std::uint32_t height, const PredictionDelivery& delivery, AnnotationBatch& annotations,
                              PredictionReadback& readback, std::span<const std::uint32_t> indices = {}, std::int64_t first_index = 0,
                              const mmltk::backend::data::DatasetLoader* compiled = nullptr) {
    annotations.demand.resize(batch_size);
    bool masks = false, pixels = false;
    for (std::size_t image = 0; image < batch_size; ++image) {
        auto demand = delivery.demand ? delivery.demand(indices.empty() ? first_index + static_cast<std::int64_t>(image) : indices[image]) : PredictionDemand{};
        demand.encoded_masks = (options.include_masks || demand.encoded_masks) && backend.has_masks();
        demand.source_pixels = (delivery.source_pixels || demand.source_pixels) && static_cast<bool>(delivery.completed);
        demand.preview_masks = demand.source_pixels && (options.include_masks || demand.preview_masks) && backend.has_masks() &&
                               width <= delivery.maximum_pixel_width && height <= delivery.maximum_pixel_height;
        annotations.demand[image] = demand;
        masks = masks || demand.encoded_masks || demand.preview_masks;
        pixels = pixels || (demand.source_pixels && width <= delivery.maximum_pixel_width && height <= delivery.maximum_pixel_height);
    }
    prepare_annotations(annotations, batch_size, backend.capacity(batch_size, width, height, masks), width, height, options.device_id, masks, pixels);
    backend.Execute(input, annotations);
    if (compiled) {
        for (std::size_t image = 0; image < batch_size; ++image) {
            const auto geometry = compiled->geometry(indices[image]);
            clip_prediction_boxes_(annotations.boxes[image], geometry.offset_x, geometry.offset_y,
                geometry.offset_x + geometry.resized_width, geometry.offset_y + geometry.resized_height);
        }
    }
    if (annotations.storage.front().value_capacity != 0U) readback.Read(annotations);
}
struct PredictionSourceStorage final {
    std::array<torch::Tensor, 11U> products;
    std::shared_ptr<void> backend_masks;
    std::shared_ptr<void> source;
};
void deliver_prediction(const PredictionDelivery& delivery, const PredictionRecord& record, PredictionPixels pixels, const AnnotationBatch& annotations,
                        std::size_t index, const PredictionReadback& readback, std::shared_ptr<void> source) {
    if (!delivery.completed) return;
    if (!annotations.demand[index].source_pixels)
        pixels = {};
    else if (!annotations.preview_failure.empty())
        pixels = {.preview_failure = annotations.preview_failure};
    else if (pixels.width > delivery.maximum_pixel_width || pixels.height > delivery.maximum_pixel_height)
        pixels = {.preview_failure = "Prediction preview exceeds the visual dimensions"};
    else if (pixels.chw || pixels.rgb8) {
        try {
            pixels.custody = std::make_shared<PredictionSourceStorage>(
                PredictionSourceStorage{{annotations.boxes, annotations.labels, annotations.scores, annotations.masks, annotations.compact_boxes,
                                         annotations.compact_labels, annotations.compact_scores, readback.device_indices, readback.index_values,
                                         annotations.selections[index].query_indices, annotations.selections[index].mask_logits.value_or(torch::Tensor{})},
                                        annotations.runtime_selections[index].custody,
                                        std::move(source)});
        } catch (...) { pixels = {.preview_failure = "Prediction source custody allocation failed"}; }
    }
    const runtime::AnalysisAnnotationStorage unavailable{.source_region = annotations.storage[index].source_region};
    const auto& current = pixels.chw || pixels.rgb8 ? annotations.storage[index] : unavailable;
    delivery.completed(record, std::move(pixels), current);
}
[[nodiscard]] std::vector<Prediction> copy_predictions(AnnotationBatch& batch, std::size_t index, float threshold, PredictionReadback& storage,
                                                       int class_count) {
    const auto count = batch.storage[index].value_count;
    if (count == 0) return {};
    const auto boxes = storage.box_values[index];
    const auto labels = storage.label_values[index];
    const auto scores = storage.score_values[index];
    auto selected_indices = storage.index_values[index].narrow(0, 0, static_cast<std::int64_t>(count));
    auto* retained = selected_indices.data_ptr<std::int64_t>();
    std::vector<Prediction> result;
    const auto* box_values = boxes.data_ptr<float>();
    const auto* label_values = labels.data_ptr<std::int32_t>();
    const auto* score_values = scores.data_ptr<float>();
    std::size_t survivors = 0U;
    for (std::size_t detection = 0U; detection < count; ++detection) {
        if (label_values[detection] < 0 || label_values[detection] >= class_count) throw std::runtime_error("RF-DETR prediction class index is out of range");
        if (score_values[detection] < threshold) { continue; }
        retained[survivors++] = static_cast<std::int64_t>(detection);
    }
    result.reserve(survivors);
    for (std::size_t survivor = 0; survivor < survivors; ++survivor) {
        const auto detection = static_cast<std::size_t>(retained[survivor]);
        result.push_back({
            .class_reference = label_values[detection],
            .score = score_values[detection],
            .bbox_xyxy =
                {
                    box_values[detection * 4U],
                    box_values[detection * 4U + 1U],
                    box_values[detection * 4U + 2U],
                    box_values[detection * 4U + 3U],
                },
            .mask = {},
            .has_mask = false,
        });
    }
    auto& annotation = batch.storage[index];
    const auto source_region = annotation.source_region;
    const auto scalar_layout = annotation;
    annotation = {};
    annotation.source_region = source_region;
    annotation.class_domain = scalar_layout.class_domain;
    annotation.class_catalog = scalar_layout.class_catalog;
    batch.preview_failure.clear();
    if (result.empty() || (!batch.raw_preview && !batch.want_masks)) return result;
    const auto active = static_cast<std::int64_t>(result.size());
    selected_indices = selected_indices.narrow(0, 0, active);
    const auto& selection = batch.selections[index];
    const auto cuda = selection.query_indices.options();
    const auto stream = c10::cuda::getCurrentCUDAStream(batch.boxes.get_device()).stream();
    PredictionRawPreparation preview(batch.raw_preview, annotation, batch.preview_failure, stream);
    const auto upload_survivors = [&] {
        if (!storage.device_indices.defined())
            storage.device_indices = torch::empty({active}, cuda);
        else
            storage.device_indices.resize_({active});
        storage.device_indices.copy_(selected_indices, true);
    };
    // Mask query selection is semantic work. BBox-only survivor upload belongs
    // entirely to optional preview preparation.
    if (batch.encoded_masks) upload_survivors();
    preview.Execute([&] {
        if (!batch.encoded_masks) upload_survivors();
        annotation = scalar_layout;
        annotation.masks = {};
        annotation.value_count = result.size();
        annotation.device_value_count = nullptr;
        annotation.completed_value_count = nullptr;
        annotation.count_custody.reset();
        // Reused compact buffers pair the callback metadata with precisely these survivors.
        const auto compact = [&](torch::Tensor& destination, const torch::Tensor& candidates) {
            if (!destination.defined()) destination = torch::empty({0}, candidates.options());
            if (candidates.dim() == 2)
                destination.resize_({active, candidates.size(1)});
            else
                destination.resize_({active});
            at::index_select_out(destination, candidates, 0, storage.device_indices);
        };
        compact(batch.compact_boxes, batch.boxes[index]);
        compact(batch.compact_labels, batch.labels[index]);
        compact(batch.compact_scores, batch.scores[index]);
        annotation.value_capacity = result.size();
        annotation.boxes_xyxy.address = reinterpret_cast<std::uintptr_t>(batch.compact_boxes.data_ptr());
        annotation.class_references.address = reinterpret_cast<std::uintptr_t>(batch.compact_labels.data_ptr());
        annotation.confidences.address = reinterpret_cast<std::uintptr_t>(batch.compact_scores.data_ptr());
        for (auto* buffer : {&annotation.boxes_xyxy, &annotation.class_references, &annotation.confidences}) {
            buffer->shape.extents[0] = static_cast<std::uint32_t>(result.size());
            buffer->capacity_bytes = result.size() * (buffer == &annotation.boxes_xyxy ? 4U * sizeof(float) : sizeof(float));
        }
    });
    if (!batch.encoded_masks && (!batch.preview_masks || !preview.available())) return result;
    const auto materialize_masks = [&] {
        const auto raw_mask_work = [&](auto&& work) {
            if (batch.encoded_masks)
                preview.Execute(std::forward<decltype(work)>(work));
            else
                std::forward<decltype(work)>(work)();
        };
        if (!selection.mask_logits) throw std::runtime_error("RF-DETR requested mask output is absent");
        if (!storage.selected_queries.defined()) storage.selected_queries = torch::empty({0}, cuda);
        storage.selected_queries.resize_({1, active});
        at::index_select_out(storage.selected_queries, selection.query_indices, 1, storage.device_indices);
        const auto width = source_region.width;
        const auto height = source_region.height;
        const auto pixels = checked_prediction_extent(width, height, kMaximumEncodedMaskPixels);
        const auto plan = PredictionMaskChunk::Resolve(result.size(), selection.mask_logits->size(2), selection.mask_logits->size(3), height, width,
                                                       selection.mask_logits->element_size());
        const auto per_chunk = plan.count;
        auto retained_capacity = storage.mask_workspace.RetainedCapacity(storage.masks.capacity_bytes());
        for (std::size_t plane = 0U; plane < retained_capacity.bytes.size(); ++plane)
            retained_capacity.bytes[plane] = std::max(retained_capacity.bytes[plane], plan.capacity.bytes[plane]);
        if (retained_capacity.Total() > plan.retained_limit) {
            // Encoded-mask reads completed their host visibility wait; preview-only
            // work has no pinned view. GPU scratch reuse stays on the inference
            // stream. Drop retained storage when mixed shapes exceed the budget.
            storage.mask_values = torch::Tensor{};
            const auto released = storage.masks.ReleaseSettled();
            if (released != CUDA_SUCCESS) throw std::runtime_error("prediction mask scratch is still borrowed");
            storage.mask_workspace.ResetSettled();
        }
        if (batch.preview_masks)
            raw_mask_work([&] {
                static_cast<void>(checked_prediction_extent(result.size(), pixels, kMaximumPredictionTensorBytes));
                if (!batch.masks.defined())
                    batch.masks = torch::empty({active, height, width}, batch.boxes.options().dtype(torch::kUInt8));
                else
                    batch.masks.resize_({active, height, width});
            });
        std::size_t remaining_runs = kMaximumPredictionMaskRuns;
        for (std::size_t start = 0; start < result.size(); start += per_chunk) {
            const auto chunk_size = std::min(per_chunk, result.size() - start);
            const auto chunk = static_cast<std::int64_t>(chunk_size);
            const auto masks = storage.mask_workspace.Materialize(
                *selection.mask_logits, storage.selected_queries.narrow(1, static_cast<std::int64_t>(start), chunk), height, width)[0];
            if (batch.preview_masks) raw_mask_work([&] { batch.masks.narrow(0, static_cast<std::int64_t>(start), chunk).copy_(masks); });
            if (!batch.encoded_masks) continue;
            storage.mask_values = torch::Tensor{};
            storage.mask_values = storage.masks.view(masks.sizes(), torch::kBool);
            storage.mask_values.copy_(masks, true);
            const auto status = cudaStreamSynchronize(c10::cuda::getCurrentCUDAStream(batch.boxes.get_device()).stream());
            if (status != cudaSuccess) throw runtime::CudaOperationError{status, "prediction mask chunk completion"};
            const auto* values = storage.mask_values.data_ptr<bool>();
            for (std::size_t offset = 0; offset < chunk_size; ++offset) {
                auto& prediction = result[start + offset];
                encode_mask_values_into(
                    height, width, prediction.mask, [values, offset, pixels](std::uint32_t pixel) { return values[offset * pixels + pixel]; }, remaining_runs);
                remaining_runs -= prediction.mask.runs.size();
                prediction.has_mask = true;
            }
        }
        annotation.masks_available = batch.preview_masks && preview.available();
        if (annotation.masks_available)
            annotation.masks = {
                .address = reinterpret_cast<std::uintptr_t>(batch.masks.data_ptr()),
                .capacity_bytes = result.size() * pixels,
                .shape = {.rank = 3U, .extents = {static_cast<std::uint32_t>(result.size()), height, width}},
                .element_type = runtime::AnalysisElementType::Uint8,
            };
    };
    if (batch.encoded_masks)
        materialize_masks();
    else
        preview.Execute(materialize_masks);
    return result;
}
using DecodedPredictionImage = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>;
void complete_prediction_record(PredictionRecord& record, std::size_t index, const PredictRequest& options, const PredictionBackend& backend,
                                AnnotationBatch& annotations, PredictionReadback& readback, const PredictionDelivery& delivery, PredictionRunResult& result,
                                std::size_t total, PredictionPixels pixels, std::shared_ptr<void> source = {},
                                DecodedPredictionImage* decoded_source = nullptr) {
    annotations.encoded_masks = annotations.demand[index].encoded_masks;
    annotations.preview_masks = annotations.demand[index].preview_masks;
    annotations.want_masks = annotations.encoded_masks || annotations.preview_masks;
    annotations.raw_preview =
        annotations.demand[index].source_pixels && pixels.width <= delivery.maximum_pixel_width && pixels.height <= delivery.maximum_pixel_height;
    record.detections = copy_predictions(
        annotations, index, options.threshold, readback,
        static_cast<int>(backend.class_layout()->semantic() ? backend.class_layout()->catalog()->size() : backend.class_layout()->output_width()));
    for (auto& detection : record.detections) {
        detection.image_id = static_cast<int>(record.image_id);
        detection.class_domain = backend.class_layout()->domain();
    }
    if (delivery.completed) {
        // Decoded images keep unique ownership until semantics are complete and
        // pixels are requested. The shared control block is optional preview work.
        if (decoded_source && annotations.demand[index].source_pixels) {
            try {
                std::shared_ptr<stbi_uc> decoded = std::move(*decoded_source);
                pixels.rgb8 = decoded.get();
                source = std::move(decoded);
            } catch (...) { pixels = {.preview_failure = "Prediction decoded source custody allocation failed"}; }
        }
        deliver_prediction(delivery, record, std::move(pixels), annotations, index, readback, std::move(source));
    }
    ++result.processed_images;
    if (delivery.progress) delivery.progress(result.processed_images, total >= result.processed_images ? total : 0U);
}
[[nodiscard]] PredictionRunResult run_image_prediction(const PredictRequest& options, PredictionBackend& backend,
                                                       const runtime::BorrowedCommandStream command_stream, PredictionRunResult result,
                                                       PredictionReadback& readback, AnnotationBatch& annotations, const PredictionDelivery& delivery) {
    const auto resolution = static_cast<int>(backend.resolution());
    auto host = mmltk::backend::ml::cuda::numa_empty({1, 3, resolution, resolution}, at::kFloat, options.device_id);
    auto device = torch::empty(host.sizes(), torch::TensorOptions().dtype(at::kFloat).device(torch::kCUDA, static_cast<c10::DeviceIndex>(options.device_id)));
    GpuBatchPreprocessor preprocessor(1, resolution, resolution, options.device_id, backend.input_type());
    mmltk::backend::imaging::resample::RgbImageResizer resizer(1);
    std::vector<std::uint8_t> resized(static_cast<std::size_t>(resolution) * resolution * 3U);
    const auto started = std::chrono::steady_clock::now();
    const auto total = options.limit_images == 0U ? options.image_inputs.size() : std::min(options.limit_images, options.image_inputs.size());
    if (delivery.begin) delivery.begin(result);
    for (std::size_t index = 0U; index < total && !delivery.stop.stop_requested(); ++index) {
        const auto& source = options.image_inputs[index];
        int width = 0;
        int height = 0;
        int channels = 0;
        DecodedPredictionImage owned_pixels(stbi_load(source.image_path.c_str(), &width, &height, &channels, 3), &stbi_image_free);
        stbi_uc* pixels = owned_pixels.get();
        if (pixels == nullptr || width <= 0 || height <= 0) {
            throw std::runtime_error("failed to decode RF-DETR prediction image: " + source.image_path.string());
        }
        const std::uint8_t* input_pixels = pixels;
        if (width != resolution || height != resolution) {
            resizer.resize(pixels, width, height, resized.data(), resolution, resolution);
            input_pixels = resized.data();
        }
        mmltk::backend::imaging::resample::rgb_hwc_u8_to_nchw_f32(input_pixels, host.data_ptr<float>(), resolution, resolution);
        device.copy_(host, true);
        const auto normalized = preprocessor.run({.num_images = 1U, .device_images = device.data_ptr<float>()});
        execute_prediction_batch(options, backend, normalized, 1U, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), delivery, annotations,
                                 readback, {}, static_cast<std::int64_t>(result.processed_images));
        preprocessor.record_consumer(reinterpret_cast<cudaStream_t>(command_stream.native_handle));
        PredictionRecord record;
        record.dataset_index = static_cast<std::int64_t>(index);
        record.image_id = source.image_id != 0 ? source.image_id : static_cast<std::int64_t>(index + 1U);
        record.source_name = source.source_name.empty() ? source.image_path.string() : source.source_name;
        complete_prediction_record(record, 0U, options, backend, annotations, readback, delivery, result, total,
                                   {.width = static_cast<std::uint32_t>(width),
                                    .height = static_cast<std::uint32_t>(height),
                                    .device = options.device_id,
                                    .stream = command_stream.native_handle},
                                   {}, &owned_pixels);
    }
    result.cancelled = delivery.stop.stop_requested();
    finish_prediction_run(result, started);
    return result;
}
[[nodiscard]] PredictionRunResult run_video_prediction(const PredictRequest& options, PredictionBackend& backend, runtime::BorrowedCommandStream command_stream,
                                                       PredictionRunResult result, PredictionReadback& readback, AnnotationBatch& annotations,
                                                       const PredictionDelivery& delivery,
                                                       const std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>& source_retirement) {
    const auto started = std::chrono::steady_clock::now();
    const auto bytes_per_pixel = checked_prediction_extent(3U, sizeof(float), kMaximumPredictionTensorBytes);
    const mmltk::backend::media::video::VideoFrameCapacity capacity{kMaximumPredictionTensorBytes / bytes_per_pixel};
    auto source = std::make_shared<mmltk::backend::media::video::VideoFileSource>(options.video_path, capacity, options.device_id, command_stream.native_handle,
                                                                                  delivery.stop, source_retirement);
    if (delivery.begin) delivery.begin(result);
    const auto cuda = torch::TensorOptions().dtype(at::kFloat).device(torch::kCUDA, options.device_id);
    const auto resolution = static_cast<std::int64_t>(backend.resolution());
    auto normalized = torch::empty({1, 3, resolution, resolution}, cuda);
    GpuBatchPreprocessor preprocessor(1, resolution, resolution, options.device_id, backend.input_type());
    while (!delivery.stop.stop_requested() && (options.limit_images == 0U || result.processed_images < options.limit_images)) {
        auto frame = source->Next();
        if (!frame) break;
        const auto known = source->frame_count();
        const auto total = options.limit_images == 0U || known == 0U ? known : std::min<std::size_t>(known, options.limit_images);
        if (delivery.decoded) delivery.decoded(frame->index + 1U, total >= frame->index + 1U ? total : 0U);
        if (delivery.before_frame && !delivery.before_frame(frame->presentation_seconds, source->frames_per_second())) break;
        const std::array<std::int64_t, 4> shape{1, 3, frame->height, frame->width};
        auto input = torch::from_blob(const_cast<float*>(frame->chw), at::IntArrayRef{shape}, cuda);
        at::upsample_bilinear2d_out(normalized, input, {resolution, resolution}, false);
        const auto model_input = preprocessor.run({.num_images = 1U, .device_images = normalized.data_ptr<float>()});
        execute_prediction_batch(options, backend, model_input, 1U, frame->width, frame->height, delivery, annotations,
                                 readback, {}, static_cast<std::int64_t>(result.processed_images));
        preprocessor.record_consumer(reinterpret_cast<cudaStream_t>(command_stream.native_handle));
        PredictionRecord record{.dataset_index = static_cast<std::int64_t>(frame->index),
                                .image_id = static_cast<std::int64_t>(frame->index + 1U),
                                .source_name = options.video_path.string()};
        complete_prediction_record(record, 0U, options, backend, annotations, readback, delivery, result, total,
                                   {frame->chw, frame->width, frame->height, options.device_id, command_stream.native_handle}, source);
    }
    result.cancelled = delivery.stop.stop_requested();
    finish_prediction_run(result, started);
    return result;
}
}  // namespace
struct PredictionSession::State final {
    std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> source_retirement =
        std::make_shared<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>(DatasetBatchLease::kSourceRetirementCapacity);
    mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement{1U};
    mmltk::frameworks::gpu::TerminalCudaRetirementLease retirement_lease = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement);
    std::unique_ptr<PredictionBackend> backend;
    std::unique_ptr<PredictionReadback> readback;
    AnnotationBatch annotations;
    ResolvedInferenceArtifact artifact{};
    std::string preset_name;
    std::uint32_t resolution = 0U;
    std::size_t maximum_detections = 0U;
    int device = -1;
    int readback_node = -1;
    std::uintptr_t command_stream = 0U;
    bool allow_fp16 = false;
    bool poisoned = false;
    [[nodiscard]] PredictionBackend& Bind(const PredictRequest& options, const ResolvedInferenceArtifact& selected, const runtime::BorrowedCommandStream stream,
                                          std::stop_token stop) {
        if (selected.admission) {
            selected.admission->RequireUnchanged(stop);
            if (!selected.admission->Matches(selected.path, options.class_layout_path))
                throw std::runtime_error("prediction admission does not match selected artifact");
        }
        const auto requested_resolution = options.resolution > 0 ? static_cast<std::uint32_t>(options.resolution) : 0U;
        if (!backend || !readback || !backend->class_artifact()->Matches(selected.path, options.class_layout_path) || artifact.kind != selected.kind ||
            artifact.backend_name != selected.backend_name || artifact.path != selected.path || preset_name != options.preset_name ||
            resolution != requested_resolution || maximum_detections != options.max_dets_per_image || device != options.device_id ||
            command_stream != stream.native_handle || allow_fp16 != options.allow_fp16) {
            auto next_artifact = selected;
            auto next_preset_name = options.preset_name;
            if (backend) {
                const auto status = backend->Close();
                if (status != cudaSuccess) throw runtime::CudaOperationError{status, "RF-DETR prediction session rebind"};
                backend.reset();
            }
            // A failed completion stays session-owned and cannot reuse stale identity.
            readback.reset();
            device = -1;
            backend = std::make_unique<PredictionBackend>(options, selected, stream, stop);
            backend->class_artifact()->RequireUnchanged(stop);
            readback = std::make_unique<PredictionReadback>(options.device_id);
            artifact = std::move(next_artifact);
            preset_name = std::move(next_preset_name);
            resolution = requested_resolution;
            maximum_detections = options.max_dets_per_image;
            device = options.device_id;
            command_stream = stream.native_handle;
            allow_fp16 = options.allow_fp16;
        }
        return *backend;
    }
    [[nodiscard]] PredictionRunResult RunResolved(const PredictRequest& options, const ResolvedInferenceArtifact& selected_artifact,
                                                  runtime::BorrowedCommandStream execution_stream, const PredictionDelivery& delivery);
};
PredictionSession::PredictionSession() : state_(std::make_shared<State>()) {}
PredictionSession::~PredictionSession() {
    static_cast<void>(Close());
    if (HasUnsafeCustody()) {
        auto state = std::move(state_);
        auto lease = std::move(state->retirement_lease);
        std::move(lease).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(state)), cudaErrorUnknown);
    }
}
bool PredictionSession::HasUnsafeCustody() const noexcept { return state_->poisoned || !state_->source_retirement->admission_open(); }
mmltk::backend::ml::runtime::RuntimeStatus PredictionSession::Close() noexcept {
    if (HasUnsafeCustody()) return static_cast<runtime::RuntimeStatus>(cudaErrorUnknown);
    if (!state_->backend) return mmltk::backend::ml::runtime::kRuntimeSuccess;
    const auto status = state_->backend->Close();
    if (status == cudaSuccess)
        state_->backend.reset();
    else
        state_->poisoned = true;
    return static_cast<mmltk::backend::ml::runtime::RuntimeStatus>(status);
}
PredictRequest finalize_predict_request(PredictRequest request) {
    validate_predict_request(request);
    const bool compiled = request.source_kind == PredictSourceKind::CompiledDataset;
    if (compiled) { request.compiled_path = std::filesystem::absolute(request.compiled_path); }
    return request;
}
PredictionRunResult run_prediction(const PredictRequest& request) {
    PredictionSession session;
    const auto stream = torch_cuda::current_torch_cuda_stream(request.device_id);
    PredictionJsonWriter writer(request);
    const auto result = session.Run(request, {.native_handle = stream, .valid = true},
                                    {
                                        .begin = [&](const auto& summary) { writer.Begin(summary); },
                                        .completed = [&](const auto& record, auto, const auto&) { writer.Append(record); },
                                    });
    if (!result.cancelled) writer.Complete();
    return result;
}
PredictionRunResult run_resolved_prediction(const PredictRequest& request, const ResolvedInferenceArtifact& artifact) {
    PredictionSession session;
    const auto stream = torch_cuda::current_torch_cuda_stream(request.device_id);
    return session.RunResolved(request, artifact, {.native_handle = stream, .valid = true});
}
PredictionRunResult PredictionSession::Run(const PredictRequest& request, const runtime::BorrowedCommandStream command_stream,
                                           const PredictionDelivery& delivery) {
    const auto options = finalize_predict_request(request);
    return RunResolved(options, resolve_inference_artifact(options, options.backend), command_stream, delivery);
}
PredictionRunResult PredictionSession::RunResolved(const PredictRequest& request, const ResolvedInferenceArtifact& artifact,
                                                   const runtime::BorrowedCommandStream command_stream, const PredictionDelivery& delivery) {
    if (!command_stream) throw std::invalid_argument("RF-DETR prediction command stream is invalid");
    const auto options = finalize_predict_request(request);
    if (HasUnsafeCustody()) throw runtime::CudaOperationError{cudaErrorUnknown, "prediction session has unobservable CUDA custody"};
    PredictionRunResult result;
    if (delivery.stop.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    struct BoundPredictionCall final {
        State* state;
        const PredictRequest* options;
        const ResolvedInferenceArtifact* artifact;
        runtime::BorrowedCommandStream command_stream;
        PredictionRunResult* result;
        const PredictionDelivery* delivery;
    } call{
        .state = state_.get(),
        // CLEANUP-IGNORE: This prediction session binds its distinct state and result across the Torch CUDA callback
        // ABI.
        .options = &options,
        .artifact = &artifact,
        .command_stream = command_stream,
        .result = &result,
        .delivery = &delivery,
    };
    try {
        torch_cuda::run_on_torch_cuda_stream(options.device_id, command_stream.native_handle, &call, [](void* opaque) {
            auto& bound = *static_cast<BoundPredictionCall*>(opaque);
            *bound.result = bound.state->RunResolved(*bound.options, *bound.artifact, bound.command_stream, *bound.delivery);
        });
    } catch (const ArtifactPublicationCancelled&) { result.cancelled = true; } catch (const mmltk::frameworks::gpu::CudaContextFailure& error) {
        if (error.terminal()) {
            state_->poisoned = true;
            throw runtime::CudaOperationError{cudaErrorUnknown, "prediction video caller context"};
        }
        throw;
    } catch (const runtime::CudaOperationError&) {
        state_->poisoned = true;
        throw;
    }
    if (HasUnsafeCustody()) throw runtime::CudaOperationError{cudaErrorUnknown, "prediction source teardown"};
    return result;
}
PredictionRunResult PredictionSession::RunAndWrite(const PredictRequest& request, const runtime::BorrowedCommandStream command_stream,
                                                   const PredictionDelivery& delivery) {
    PredictionJsonWriter writer(request);
    auto combined = delivery;
    combined.begin = [&](const auto& summary) {
        writer.Begin(summary);
        if (delivery.begin) delivery.begin(summary);
    };
    combined.completed = [&](const auto& record, auto pixels, const auto& annotations) {
        writer.Append(record);
        if (delivery.completed) delivery.completed(record, pixels, annotations);
    };
    auto result = Run(request, command_stream, combined);
    // This is the operation's completion claim: later Stop cannot invalidate
    // a successfully committed file. Any close/rename error still fails the run.
    result.cancelled = result.cancelled || delivery.stop.stop_requested();
    if (!result.cancelled) writer.Complete();
    return result;
}
PredictionRunResult PredictionSession::State::RunResolved(const PredictRequest& options, const ResolvedInferenceArtifact& selected_artifact,
                                                          const runtime::BorrowedCommandStream execution_stream, const PredictionDelivery& delivery) {
    const auto execution = mmltk::frameworks::gpu::resolve_device_execution(options.device_id, mmltk::common::system::NumaTopology::Capture(),
                                                                            options.numa_node, options.cpu_affinity);
    const auto& placement = execution.placement;
    std::shared_ptr<mmltk::backend::data::DatasetLoader> loader =
        options.source_kind != PredictSourceKind::CompiledDataset
            ? std::unique_ptr<mmltk::backend::data::DatasetLoader>{}
            : inference_detail::make_loader(options.compiled_path, options.batch_size, options, 2U, source_retirement);
    mmltk::common::system::ScopedExecutionPolicy policy({placement.cpus, "predict", 0, placement.numa_node, -10, false});
    if (readback_node != placement.numa_node) {
        readback = std::make_unique<PredictionReadback>(options.device_id);
        readback_node = placement.numa_node;
    }
    auto& bound_backend = Bind(options, selected_artifact, execution_stream, delivery.stop);
    PredictionRunResult result;
    result.backend_name = bound_backend.name();
    result.artifacts = bound_backend.artifacts();
    result.artifacts.class_layout = bound_backend.class_layout()->record();
    result.candidate_count = bound_backend.candidate_count();
    result.masks_available = bound_backend.has_masks();
    result.class_catalog = bound_backend.class_layout()->catalog();
    result.class_domain = bound_backend.class_layout()->domain();
    if (options.source_kind == PredictSourceKind::VideoFile)
        return run_video_prediction(options, bound_backend, execution_stream, std::move(result), *readback, annotations, delivery, source_retirement);
    if (options.source_kind == PredictSourceKind::ImageFiles) {
        return run_image_prediction(options, bound_backend, execution_stream, std::move(result), *readback, annotations, delivery);
    }
    if (loader->image_width() != bound_backend.resolution() || loader->image_height() != bound_backend.resolution()) {
        throw std::invalid_argument("compiled dataset resolution does not match RF-DETR artifact");
    }
    if (delivery.begin) delivery.begin(result);
    const auto total = options.limit_images == 0U ? loader->num_images() : std::min(options.limit_images, loader->num_images());
    const auto image_ids = EvaluationDatasetOwner(*loader, EvaluationMetricSet::BBox).image_ids();
    GpuBatchPreprocessor preprocessor(static_cast<std::int64_t>(options.batch_size), static_cast<int>(loader->image_height()),
                                            static_cast<int>(loader->image_width()), options.device_id, bound_backend.input_type());
    const auto started = std::chrono::steady_clock::now();
    loader->begin_epoch();
    mmltk::backend::data::Batch batch{};
    std::shared_ptr<DatasetBatchLease> batch_lease;
    while (result.processed_images < total && !delivery.stop.stop_requested()) {
        // Allocate custody before checking out a slot; ordinary batches reuse it.
        if (!batch_lease || batch_lease.use_count() != 1)
            batch_lease = std::make_shared<DatasetBatchLease>(loader, execution_stream.native_handle, source_retirement);
        if (!loader->next_batch(batch, delivery.stop)) break;
        batch_lease->Adopt(batch);
        loader->wait_batch(batch);
        if (delivery.stop.stop_requested()) {
            batch_lease->Release();
            break;
        }
        auto active_batch = batch;
        active_batch.num_images = std::min(batch.num_images, total - result.processed_images);
        auto normalized = preprocessor.run(active_batch);
        execute_prediction_batch(options, bound_backend, normalized, active_batch.num_images, loader->image_width(), loader->image_height(), delivery,
                                 annotations, *readback, {batch.image_indices, active_batch.num_images}, 0, loader.get());
        preprocessor.record_consumer(reinterpret_cast<cudaStream_t>(execution_stream.native_handle));
        for (std::size_t image = 0U; image < active_batch.num_images && !delivery.stop.stop_requested(); ++image) {
            PredictionRecord record;
            record.dataset_index = static_cast<std::int64_t>(batch.image_indices[image]);
            const auto dataset_index = static_cast<std::size_t>(record.dataset_index);
            if (dataset_index >= image_ids.size()) { throw std::runtime_error("RF-DETR dataset image index is out of range"); }
            record.image_id = image_ids[dataset_index];
            record.source_name = std::to_string(record.dataset_index);
            complete_prediction_record(record, image, options, bound_backend, annotations, *readback, delivery, result, total,
                                       {.chw = batch.device_images + image * loader->image_stride() / sizeof(float),
                                        .width = loader->image_width(),
                                        .height = loader->image_height(),
                                        .device = options.device_id,
                                        .stream = execution_stream.native_handle,
                                        .stop_source = [](void* owner) { static_cast<DatasetBatchLease*>(owner)->StopWorkers(); },
                                        .source_control = batch_lease.get()},
                                       batch_lease);
        }
        batch_lease->Release();
    }
    result.cancelled = delivery.stop.stop_requested();
    finish_prediction_run(result, started);
    return result;
}
struct PredictionJsonWriter::State final {
    PredictRequest request;
    std::filesystem::path temporary;
    std::ofstream stream;
    std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> class_catalog;
    bool first = true;
    bool begun = false;
    bool committed = false;
};
PredictionJsonWriter::PredictionJsonWriter(const PredictRequest& request) : state_(std::make_unique<State>()) { state_->request = request; }
PredictionJsonWriter::~PredictionJsonWriter() {
    try {
        if (state_->stream.is_open()) state_->stream.close();
    } catch (...) {}
    if (!state_->temporary.empty() && !state_->committed) {
        std::error_code error;
        std::filesystem::remove(state_->temporary, error);
    }
}
void PredictionJsonWriter::Begin(const PredictionRunResult& result) {
    if (state_->request.output_path.empty()) return;
    if (state_->begun) throw std::logic_error("prediction JSON was already started");
    static std::atomic<std::uint64_t> sequence{0U};
    state_->temporary = state_->request.output_path.string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(++sequence);
    state_->stream.exceptions(std::ios::badbit | std::ios::failbit);
    state_->stream.open(state_->temporary);
    state_->class_catalog = result.class_catalog;
    nlohmann::json output = {
        {"source_kind", predict_source_kind_name(state_->request.source_kind)},
        {"model_kind", result.artifacts.input_kind},
        {"model_path", result.artifacts.input_path.string()},
        {"preset_name", result.artifacts.config.preset_name},
        {"backend", result.backend_name},
        {"mask_rle_encoding", "row_major_start_length"},
        {"class_domain", result.class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::Foreground ? "foreground" : "raw_output_slot"},
        {"class_layout", nlohmann::json::parse(encode_class_layout(result.artifacts.class_layout))},
    };
    if (state_->request.source_kind == PredictSourceKind::CompiledDataset) {
        output["compiled_path"] = state_->request.compiled_path.string();
    } else if (state_->request.source_kind == PredictSourceKind::VideoFile) {
        output["video_path"] = state_->request.video_path.string();
    } else {
        output["input_image_count"] = state_->request.image_inputs.size();
    }
    if (!result.artifacts.weights_path.empty()) { output["weights_path"] = result.artifacts.weights_path.string(); }
    if (!result.artifacts.onnx_path.empty()) { output["onnx_path"] = result.artifacts.onnx_path.string(); }
    if (!result.artifacts.tensorrt_path.empty()) { output["tensorrt_path"] = result.artifacts.tensorrt_path.string(); }
    auto prefix = output.dump();
    prefix.pop_back();
    state_->stream << prefix << ",\"records\":[";
    state_->begun = true;
}
void PredictionJsonWriter::Append(const PredictionRecord& record) {
    if (state_->request.output_path.empty()) return;
    if (!state_->begun || state_->committed) throw std::logic_error("prediction JSON is not writable");
    if (!state_->first) state_->stream << ',';
    state_->stream << prediction_record_json(record, state_->class_catalog ? state_->class_catalog->names() : std::span<const std::string>{}).dump();
    state_->first = false;
}
void PredictionJsonWriter::Complete() {
    if (state_->request.output_path.empty()) return;
    if (!state_->begun || state_->committed) throw std::logic_error("prediction JSON is not open");
    state_->stream << "]}\n";
    state_->stream.close();
    std::filesystem::rename(state_->temporary, state_->request.output_path);
    state_->committed = true;
}
void print_prediction_summary(const PredictRequest&, const PredictionRunResult& result) {
    std::cout << result.backend_name << ": " << result.processed_images << " images, " << result.timing.img_per_s << " img/s\n";
}
}  // namespace mmltk::backend::models::rfdetr
