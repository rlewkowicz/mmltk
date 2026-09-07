module;
#include "src/backend/ml/cuda/numa_host_tensor.h"
#include <cuda_runtime.h>
#include <c10/cuda/CUDAStream.h>
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/device_execution.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "detection_types.h"
#include "execution_precision.h"
#include "model_technical.h"
#include "postprocess.h"
#include "scalar_type_utils.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/data/image_resize.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/backend/models/rfdetr/core/model_info.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "stb_image.h"
#include "torch_api.h"
// CLEANUP-IGNORE: This module declaration terminates Prediction's private global fragment.
module mmltk.backend.models.rfdetr.inference.prediction;

// CLEANUP-IGNORE: This inference implementation imports the concrete owners used by its typed prediction boundary.

import mmltk.backend.ml.cuda.torch_scope;
import mmltk.backend.models.rfdetr.core.artifact_resolution; // CLEANUP-IGNORE: Prediction directly imports its concrete RF-DETR implementation owners.
import mmltk.backend.models.rfdetr.core.dataset_limit_resolution;
import mmltk.backend.models.rfdetr.core.dataset_utils;
import mmltk.backend.models.rfdetr.core.runtime;
import mmltk.backend.models.rfdetr.core.tool_launch_utils;
import mmltk.backend.models.rfdetr.core.evaluator;
import mmltk.backend.models.rfdetr.core.model;

import mmltk.backend.models.rfdetr.inference.loader;

#include "model_access.h"
#include "model_state_access.h"

namespace mmltk::backend::models::rfdetr {
namespace runtime = mmltk::backend::ml::runtime;
namespace tensor_api = mmltk::backend::ml::torch_api;
namespace torch_cuda = mmltk::backend::ml::cuda;

namespace {

class InferenceBatchPreprocessor final {
   public:
    InferenceBatchPreprocessor(const std::int64_t capacity, const int height, const int width, const int device,
                               const tensor_api::ScalarType type)
        : capacity_(capacity),
          height_(height),
          width_(width),
          device_(device),
          output_(tensor_api::empty({capacity, 3, height, width}, tensor_api::TensorOptions().dtype(type).device(
                                                                      tensor_api::kCUDA, static_cast<tensor_api::DeviceIndex>(device)))) {}

    [[nodiscard]] tensor_api::Tensor Run(const mmltk::backend::data::Batch& batch) {
        const auto active = static_cast<std::int64_t>(batch.num_images);
        if (active <= 0 || active > capacity_ || batch.device_images == nullptr) {
            throw std::invalid_argument("invalid RF-DETR inference preprocessing batch");
        }
        const std::array<std::int64_t, 4> shape{active, 3, height_, width_};
        const auto input = tensor_api::from_blob(
            const_cast<float*>(batch.device_images), tensor_api::IntArrayRef{shape},
            tensor_api::TensorOptions().dtype(tensor_api::kUInt8).device(tensor_api::kCUDA, static_cast<tensor_api::DeviceIndex>(device_)));
        auto result = output_.narrow(0, 0, active);
        result.copy_(input.to(output_.scalar_type()).div_(255.0));
        return result;
    }

   private:
    std::int64_t capacity_;
    int height_;
    int width_;
    int device_;
    tensor_api::Tensor output_;
};

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
        case PredictSourceKind::CompiledDataset:
            return "compiled_dataset";
        case PredictSourceKind::ImageFiles:
            return "image_files";
    }
    throw std::invalid_argument("invalid RF-DETR prediction source kind");
}

[[nodiscard]] std::string encode_mask_rle(const EncodedMask& mask) {
    std::string encoded;
    for (std::size_t index = 0U; index < mask.runs.size(); ++index) {
        if (index != 0U) { encoded.push_back(' '); }
        encoded += std::to_string(mask.runs[index].first);
        encoded.push_back(':');
        encoded += std::to_string(mask.runs[index].second);
    }
    return encoded;
}

[[nodiscard]] nlohmann::json prediction_record_json(const PredictionRecord& record, const std::vector<std::string>& class_names) {
    nlohmann::json detections = nlohmann::json::array();
    for (const Prediction& prediction : record.detections) {
        const int class_index = prediction.category_id - 1;
        std::string label = std::to_string(prediction.category_id);
        if (class_index >= 0 && static_cast<std::size_t>(class_index) < class_names.size()) {
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

[[nodiscard]] runtime::RuntimeElementType runtime_type(const tensor_api::ScalarType type) {
    if (type == tensor_api::kFloat) { return runtime::RuntimeElementType::Float32; }
    if (type == tensor_api::kHalf) { return runtime::RuntimeElementType::Float16; }
    throw std::invalid_argument("RF-DETR input must be float16 or float32");
}

[[nodiscard]] runtime::RuntimeTensorBuffer input_buffer(const tensor_api::Tensor& input) {
    runtime::RuntimeShape shape{.rank = static_cast<std::uint8_t>(input.dim())};
    if (input.dim() > static_cast<std::int64_t>(runtime::kMaximumRuntimeRank)) {
        throw std::invalid_argument("RF-DETR input rank exceeds runtime bound");
    }
    for (std::int64_t index = 0; index < input.dim(); ++index) {
        shape.extents[static_cast<std::size_t>(index)] = input.size(index);
    }
    return {
        .device_data = input.data_ptr(),
        .capacity_bytes = static_cast<std::size_t>(input.numel() * input.element_size()),
        .shape = shape,
        .element_type = runtime_type(input.scalar_type()),
    };
}

struct AnnotationBatch final {
    tensor_api::Tensor boxes;
    tensor_api::Tensor labels;
    tensor_api::Tensor scores;
    std::vector<runtime::AnalysisAnnotationStorage> storage;
};

class PredictionBackend final {
   public:
    PredictionBackend(const PredictRequest& options, ResolvedInferenceArtifact artifact,
                      const runtime::BorrowedCommandStream command_stream)
        : artifact_(std::move(artifact)),
          maximum_detections_(options.max_dets_per_image),
          device_(options.device_id),
          command_stream_(command_stream) {
        switch (artifact_.kind) {
            case InferenceArtifactKind::Weights: {
                auto resolved = resolve_model_state(artifact_.path, options.preset_name, options.resolution);
                artifacts_ = std::move(resolved.artifacts);
                native_ = std::make_unique<NativeRfDetrModel>(artifacts_.config);
                auto& technical_model = detail::native_model_owner(*native_);
                static_cast<void>(technical_model.load_normalized_state(detail::model_state_owner(resolved.model_state).entries, false));
                technical_model.module().to(tensor_api::Device(tensor_api::kCUDA, static_cast<tensor_api::DeviceIndex>(device_)));
                technical_model.module().eval();
                native_precision_ =
                    options.allow_fp16 ? torch_cuda::preferred_torch_cuda_precision(device_) : torch_cuda::TorchCudaPrecision::Float32;
                switch (native_precision_) {
                    case torch_cuda::TorchCudaPrecision::Float32:
                        native_input_type_ = tensor_api::kFloat;
                        break;
                    case torch_cuda::TorchCudaPrecision::Float16:
                        native_input_type_ = tensor_api::kHalf;
                        break;
                    case torch_cuda::TorchCudaPrecision::BFloat16:
                        native_input_type_ = tensor_api::kBFloat16;
                        break;
                }
                native_autocast_enabled_ = options.allow_fp16;
                resolution_ = static_cast<std::uint32_t>(artifacts_.config.resolution);
                return;
            }
            case InferenceArtifactKind::Onnx:
            case InferenceArtifactKind::TensorRt:
                break;
            default:
                throw std::invalid_argument("invalid RF-DETR inference artifact kind");
        }
        runtime_ = make_rfdetr_runtime_backend({
            .artifacts = options,
            .backend = artifact_.backend_name,
            .device = options.device_id,
            .command_stream = command_stream,
            .static_resolution = options.resolution > 0 ? static_cast<std::uint32_t>(options.resolution) : 0U,
            .maximum_detections = options.max_dets_per_image,
            .save_compiled_model_path = {},
            .allow_fp16 = options.allow_fp16,
        });
        resolution_ = runtime_->static_resolution();
        artifacts_ = describe_inference_artifact(options, artifact_, resolution_);
        for (std::size_t index = 0U; index < runtime_->model_info().output_count; ++index) {
            const auto& output = runtime_->model_info().outputs[index];
            if (output.name.find("logit") == std::string::npos) { continue; }
            artifacts_.config.num_queries = static_cast<int>(output.shape.extents[1]);
            artifacts_.config.num_select =
                static_cast<int>(std::min<std::int64_t>(output.shape.extents[1], static_cast<std::int64_t>(maximum_detections_)));
            artifacts_.config.num_classes = static_cast<int>(output.shape.extents[2]);
            artifacts_.source_num_queries = artifacts_.config.num_queries;
            artifacts_.source_num_select = artifacts_.config.num_select;
            break;
        }
    }

    ~PredictionBackend() { static_cast<void>(Close()); }

    [[nodiscard]] const std::string& name() const noexcept { return artifact_.backend_name; }
    [[nodiscard]] std::uint32_t resolution() const noexcept { return resolution_; }
    [[nodiscard]] tensor_api::ScalarType input_type() const noexcept {
        if (runtime_ && runtime_->model_info().input.element_type == runtime::RuntimeElementType::Float16) { return tensor_api::kHalf; }
        return native_ ? native_input_type_ : tensor_api::kFloat;
    }
    [[nodiscard]] const ResolvedModelArtifacts& artifacts() const noexcept { return artifacts_; }

    void Execute(const tensor_api::Tensor& input, AnnotationBatch& annotations) {
        if (runtime_) {
            auto submission = runtime_->Run(input_buffer(input), annotations.storage);
            runtime_->ReleaseAfterCompletion(std::move(submission));
            return;
        }
        struct NativeCall final {
            PredictionBackend* owner;
            const tensor_api::Tensor* input;
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
        const auto completed = cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(command_stream_.native_handle));
        if (completed != cudaSuccess) {
            for (auto& storage : annotations.storage) {
                storage.value_count = 0U;
            }
            throw runtime::CudaOperationError{completed, "native RF-DETR inference completion"};
        }
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
            native_.reset();
        }
        return runtime::kRuntimeSuccess;
    }

   private:
    void ExecuteNative(const tensor_api::Tensor& input, AnnotationBatch& annotations) {
        auto mask = tensor_api::zeros({input.size(0), input.size(2), input.size(3)}, input.options().dtype(tensor_api::kBool));
        const ModelOutputs outputs = detail::native_model_owner(*native_).forward(NestedTensor{input, std::move(mask)}, false);
        assert_inference_output_dtype(outputs.main.pred_logits, outputs.main.pred_boxes, native_input_type_, "native RF-DETR inference");
        for (std::size_t index = 0U; index < annotations.storage.size(); ++index) {
            auto& storage = annotations.storage[index];
            const auto count = std::min({maximum_detections_, storage.value_capacity,
                                         static_cast<std::size_t>(outputs.main.pred_logits.size(1) * outputs.main.pred_logits.size(2))});
            const auto processed = postprocess_output_batch_fixed_size(
                OutputTensors{
                    .pred_logits = outputs.main.pred_logits.narrow(0, static_cast<std::int64_t>(index), 1),
                    .pred_boxes = outputs.main.pred_boxes.narrow(0, static_cast<std::int64_t>(index), 1),
                    .pred_masks = std::nullopt,
                },
                storage.source_region.height, storage.source_region.width, static_cast<std::int64_t>(count));
            const auto active = static_cast<std::int64_t>(count);
            annotations.boxes[index].narrow(0, 0, active).copy_(processed.boxes[0].to(tensor_api::kFloat));
            annotations.labels[index].narrow(0, 0, active).copy_(processed.labels[0].to(tensor_api::kInt));
            annotations.scores[index].narrow(0, 0, active).copy_(processed.scores[0].to(tensor_api::kFloat));
            storage.value_count = count;
        }
    }

    ResolvedInferenceArtifact artifact_;
    ResolvedModelArtifacts artifacts_;
    std::shared_ptr<RfdetrRuntimeBackend> runtime_;
    std::unique_ptr<NativeRfDetrModel> native_;
    std::size_t maximum_detections_ = 0U;
    std::uint32_t resolution_ = 0U;
    int device_ = 0;
    runtime::BorrowedCommandStream command_stream_{};
    tensor_api::ScalarType native_input_type_ = tensor_api::kFloat;
    torch_cuda::TorchCudaPrecision native_precision_ = torch_cuda::TorchCudaPrecision::Float32;
    bool native_autocast_enabled_ = false;
};

class DatasetBatchLease final {
   public:
    DatasetBatchLease(mmltk::backend::data::DatasetLoader& loader, const mmltk::backend::data::Batch& batch) noexcept
        : loader_(loader), batch_(batch) {}
    ~DatasetBatchLease() noexcept {
        if (active_) {
            try {
                loader_.release_batch(batch_);
            } catch (...) {}
        }
    }
    DatasetBatchLease(const DatasetBatchLease&) = delete;
    DatasetBatchLease& operator=(const DatasetBatchLease&) = delete;

    void Release() {
        loader_.release_batch(batch_);
        active_ = false;
    }

   private:
    mmltk::backend::data::DatasetLoader& loader_;
    const mmltk::backend::data::Batch& batch_;
    bool active_ = true;
};

[[nodiscard]] AnnotationBatch make_annotations(std::size_t batch, std::size_t capacity, std::uint32_t width, std::uint32_t height,
                                               int device) {
    const auto cuda = tensor_api::TensorOptions().device(tensor_api::kCUDA, static_cast<tensor_api::DeviceIndex>(device));
    AnnotationBatch result;
    result.boxes =
        tensor_api::empty({static_cast<std::int64_t>(batch), static_cast<std::int64_t>(capacity), 4}, cuda.dtype(tensor_api::kFloat));
    result.labels =
        tensor_api::empty({static_cast<std::int64_t>(batch), static_cast<std::int64_t>(capacity)}, cuda.dtype(tensor_api::kInt));
    result.scores =
        tensor_api::empty({static_cast<std::int64_t>(batch), static_cast<std::int64_t>(capacity)}, cuda.dtype(tensor_api::kFloat));
    result.storage.resize(batch);
    for (std::size_t index = 0U; index < batch; ++index) {
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
            .category_ids =
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
    return result;
}

struct PredictionReadback final {
    explicit PredictionReadback(int device) : boxes(device), labels(device), scores(device) {}
    mmltk::backend::ml::cuda::NumaHostTensor boxes, labels, scores;
};

[[nodiscard]] std::vector<Prediction> copy_predictions(const AnnotationBatch& batch, std::size_t index, float threshold,
                                                       PredictionReadback& storage) {
    const auto count = batch.storage[index].value_count;
    auto boxes = storage.boxes.view({static_cast<std::int64_t>(count), 4}, tensor_api::kFloat);
    auto labels = storage.labels.view({static_cast<std::int64_t>(count)}, tensor_api::kInt);
    auto scores = storage.scores.view({static_cast<std::int64_t>(count)}, tensor_api::kFloat);
    boxes.copy_(batch.boxes[index].narrow(0, 0, count), true);
    labels.copy_(batch.labels[index].narrow(0, 0, count), true);
    scores.copy_(batch.scores[index].narrow(0, 0, count), true);
    const auto stream = c10::cuda::getCurrentCUDAStream(batch.boxes[index].get_device());
    const auto status = cudaStreamSynchronize(stream.stream());
    if (status != cudaSuccess) throw runtime::CudaOperationError{status, "prediction readback completion"};
    std::vector<Prediction> result;
    result.reserve(count);
    const auto* box_values = boxes.data_ptr<float>();
    const auto* label_values = labels.data_ptr<std::int32_t>();
    const auto* score_values = scores.data_ptr<float>();
    for (std::size_t detection = 0U; detection < count; ++detection) {
        if (score_values[detection] < threshold) { continue; }
        result.push_back({
            .category_id = label_values[detection],
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
    return result;
}

[[nodiscard]] PredictionRunResult run_image_prediction(const PredictRequest& options, PredictionBackend& backend,
                                                       const runtime::BorrowedCommandStream command_stream, PredictionRunResult result,
                                                       PredictionReadback& readback) {
    const auto resolution = static_cast<int>(backend.resolution());
    auto host = mmltk::backend::ml::cuda::numa_empty({1, 3, resolution, resolution}, tensor_api::kFloat, options.device_id);
    auto device = tensor_api::empty(host.sizes(), tensor_api::TensorOptions()
                                                      .dtype(tensor_api::kFloat)
                                                      .device(tensor_api::kCUDA, static_cast<tensor_api::DeviceIndex>(options.device_id)));
    const auto mean = tensor_api::tensor({0.485F, 0.456F, 0.406F}, device.options()).view({1, 3, 1, 1});
    const auto deviation = tensor_api::tensor({0.229F, 0.224F, 0.225F}, device.options()).view({1, 3, 1, 1});
    mmltk::backend::data::RgbImageResizer resizer(1);
    std::vector<std::uint8_t> resized(static_cast<std::size_t>(resolution) * resolution * 3U);
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0U; index < options.image_inputs.size(); ++index) {
        const auto& source = options.image_inputs[index];
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load(source.image_path.c_str(), &width, &height, &channels, 3);
        if (pixels == nullptr || width <= 0 || height <= 0) {
            if (pixels != nullptr) { stbi_image_free(pixels); }
            throw std::runtime_error("failed to decode RF-DETR prediction image: " + source.image_path.string());
        }
        const std::uint8_t* input_pixels = pixels;
        if (width != resolution || height != resolution) {
            resizer.resize(pixels, width, height, resized.data(), resolution, resolution);
            input_pixels = resized.data();
        }
        mmltk::backend::data::rgb_hwc_u8_to_nchw_f32(input_pixels, host.data_ptr<float>(), resolution, resolution);
        stbi_image_free(pixels);
        device.copy_(host, true);
        auto normalized = device.sub(mean).div(deviation);
        if (backend.input_type() != tensor_api::kFloat) { normalized = normalized.to(backend.input_type()); }
        const auto preprocessed = cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(command_stream.native_handle));
        if (preprocessed != cudaSuccess) throw runtime::CudaOperationError{preprocessed, "RF-DETR image preprocessing completion"};
        auto annotations = make_annotations(1U, options.max_dets_per_image, static_cast<std::uint32_t>(width),
                                            static_cast<std::uint32_t>(height), options.device_id);
        backend.Execute(normalized, annotations);
        PredictionRecord record;
        record.dataset_index = static_cast<std::int64_t>(index);
        record.image_id = source.image_id != 0 ? source.image_id : static_cast<std::int64_t>(index + 1U);
        record.source_name = source.source_name.empty() ? source.image_path.string() : source.source_name;
        record.detections = copy_predictions(annotations, 0U, options.threshold, readback);
        for (auto& detection : record.detections) {
            detection.image_id = static_cast<int>(record.image_id);
        }
        result.records.push_back(std::move(record));
        ++result.processed_images;
    }
    finish_prediction_run(result, started);
    return result;
}

}  // namespace

struct PredictionSession::State final {
    std::unique_ptr<PredictionBackend> backend;
    std::unique_ptr<PredictionReadback> readback;
    ResolvedInferenceArtifact artifact{};
    std::string preset_name;
    std::uint32_t resolution = 0U;
    std::size_t maximum_detections = 0U;
    int device = -1;
    int readback_node = -1;
    std::uintptr_t command_stream = 0U;
    bool allow_fp16 = false;

    [[nodiscard]] PredictionBackend& Bind(const PredictRequest& options, const ResolvedInferenceArtifact& selected,
                                          const runtime::BorrowedCommandStream stream) {
        const auto requested_resolution = options.resolution > 0 ? static_cast<std::uint32_t>(options.resolution) : 0U;
        if (!backend || !readback || artifact.kind != selected.kind || artifact.backend_name != selected.backend_name ||
            artifact.path != selected.path || preset_name != options.preset_name || resolution != requested_resolution ||
            maximum_detections != options.max_dets_per_image || device != options.device_id || command_stream != stream.native_handle ||
            allow_fp16 != options.allow_fp16) {
            if (backend) {
                const auto status = backend->Close();
                if (status != cudaSuccess) throw runtime::CudaOperationError{status, "RF-DETR prediction session rebind"};
                backend.reset();
            }
            backend = std::make_unique<PredictionBackend>(options, selected, stream);
            readback = std::make_unique<PredictionReadback>(options.device_id);
            artifact = selected;
            preset_name = options.preset_name;
            resolution = requested_resolution;
            maximum_detections = options.max_dets_per_image;
            device = options.device_id;
            command_stream = stream.native_handle;
            allow_fp16 = options.allow_fp16;
        }
        return *backend;
    }

    [[nodiscard]] PredictionRunResult RunResolved(const PredictRequest& options, const ResolvedInferenceArtifact& selected_artifact,
                                                  runtime::BorrowedCommandStream execution_stream);
};

PredictionSession::PredictionSession() : state_(std::make_unique<State>()) {}
PredictionSession::~PredictionSession() { static_cast<void>(Close()); }

mmltk::backend::ml::runtime::RuntimeStatus PredictionSession::Close() noexcept {
    if (!state_->backend) return mmltk::backend::ml::runtime::kRuntimeSuccess;
    const auto status = state_->backend->Close();
    if (status == cudaSuccess) state_->backend.reset();
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
    return session.Run(request, {.native_handle = stream, .valid = true});
}

PredictionRunResult run_resolved_prediction(const PredictRequest& request, const ResolvedInferenceArtifact& artifact) {
    PredictionSession session;
    const auto stream = torch_cuda::current_torch_cuda_stream(request.device_id);
    return session.RunResolved(request, artifact, {.native_handle = stream, .valid = true});
}

PredictionRunResult PredictionSession::Run(const PredictRequest& request, const runtime::BorrowedCommandStream command_stream) {
    const auto options = finalize_predict_request(request);
    return RunResolved(options, resolve_inference_artifact(options, options.backend), command_stream);
}

PredictionRunResult PredictionSession::RunResolved(const PredictRequest& request, const ResolvedInferenceArtifact& artifact,
                                                   const runtime::BorrowedCommandStream command_stream) {
    if (!command_stream) throw std::invalid_argument("RF-DETR prediction command stream is invalid");
    const auto options = finalize_predict_request(request);
    PredictionRunResult result;
    struct BoundPredictionCall final {
        State* state;
        const PredictRequest* options;
        const ResolvedInferenceArtifact* artifact;
        runtime::BorrowedCommandStream command_stream;
        PredictionRunResult* result;
    } call{
        .state = state_.get(),
        // CLEANUP-IGNORE: This prediction session binds its distinct state and result across the Torch CUDA callback
        // ABI.
        .options = &options,
        .artifact = &artifact,
        .command_stream = command_stream,
        .result = &result,
    };
    torch_cuda::run_on_torch_cuda_stream(options.device_id, command_stream.native_handle, &call, [](void* opaque) {
        auto& bound = *static_cast<BoundPredictionCall*>(opaque);
        *bound.result = bound.state->RunResolved(*bound.options, *bound.artifact, bound.command_stream);
    });
    return result;
}

std::size_t PredictionSession::RunAndWrite(const PredictRequest& request, const runtime::BorrowedCommandStream command_stream) {
    const auto result = Run(request, command_stream);
    write_prediction_json(request, result);
    return result.processed_images;
}

PredictionRunResult PredictionSession::State::RunResolved(const PredictRequest& options, const ResolvedInferenceArtifact& selected_artifact,
                                                          const runtime::BorrowedCommandStream execution_stream) {
    const auto execution = mmltk::frameworks::gpu::resolve_device_execution(
        options.device_id, mmltk::common::system::NumaTopology::Capture(), options.numa_node, options.cpu_affinity);
    const auto& placement = execution.placement;
    auto loader = options.source_kind == PredictSourceKind::ImageFiles
                      ? std::unique_ptr<mmltk::backend::data::DatasetLoader>{}
                      : inference_detail::make_loader(options.compiled_path, options.batch_size, options, 2U);
    mmltk::common::system::ScopedExecutionPolicy policy({placement.cpus, "predict", 0, placement.numa_node, -10, false});
    if (readback_node != placement.numa_node) {
        readback = std::make_unique<PredictionReadback>(options.device_id);
        readback_node = placement.numa_node;
    }
    auto& bound_backend = Bind(options, selected_artifact, execution_stream);
    PredictionRunResult result;
    result.backend_name = bound_backend.name();
    result.artifacts = bound_backend.artifacts();
    if (options.source_kind == PredictSourceKind::ImageFiles) {
        return run_image_prediction(options, bound_backend, execution_stream, std::move(result), *readback);
    }
    if (loader->image_width() != bound_backend.resolution() || loader->image_height() != bound_backend.resolution()) {
        throw std::invalid_argument("compiled dataset resolution does not match RF-DETR artifact");
    }

    for (std::uint32_t index = 0U; index < loader->num_classes(); ++index) {
        result.class_names.emplace_back(loader->class_name(index));
    }
    const auto image_ids = EvaluationDatasetOwner(*loader, EvaluationMetricSet::BBox).image_ids();
    InferenceBatchPreprocessor preprocessor(static_cast<std::int64_t>(options.batch_size), static_cast<int>(loader->image_height()),
                                            static_cast<int>(loader->image_width()), options.device_id, bound_backend.input_type());

    const auto started = std::chrono::steady_clock::now();
    loader->begin_epoch();
    mmltk::backend::data::Batch batch{};
    while (loader->next_batch(batch)) {
        DatasetBatchLease batch_lease(*loader, batch);
        loader->wait_batch(batch);
        auto normalized = preprocessor.Run(batch);
        const auto preprocessed = cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(execution_stream.native_handle));
        if (preprocessed != cudaSuccess) throw runtime::CudaOperationError{preprocessed, "RF-DETR preprocessing completion"};
        auto annotations = make_annotations(batch.num_images, options.max_dets_per_image, loader->image_width(), loader->image_height(),
                                            options.device_id);
        bound_backend.Execute(normalized, annotations);
        for (std::size_t image = 0U; image < batch.num_images; ++image) {
            PredictionRecord record;
            record.dataset_index = static_cast<std::int64_t>(batch.image_indices[image]);
            const auto dataset_index = static_cast<std::size_t>(record.dataset_index);
            if (dataset_index >= image_ids.size()) { throw std::runtime_error("RF-DETR dataset image index is out of range"); }
            record.image_id = image_ids[dataset_index];
            record.source_name = std::to_string(record.dataset_index);
            record.detections = copy_predictions(annotations, image, options.threshold, *readback);
            for (auto& detection : record.detections) {
                detection.image_id = static_cast<int>(record.image_id);
            }
            result.records.push_back(std::move(record));
        }
        result.processed_images += batch.num_images;
        batch_lease.Release();
    }
    finish_prediction_run(result, started);
    return result;
}

void write_prediction_json(const PredictRequest& request, const PredictionRunResult& result) {
    if (request.output_path.empty()) { return; }

    nlohmann::json records = nlohmann::json::array();
    for (const auto& record : result.records) {
        records.push_back(prediction_record_json(record, result.class_names));
    }

    nlohmann::json output = {
        {"source_kind", predict_source_kind_name(request.source_kind)},
        {"model_kind", result.artifacts.input_kind},
        {"model_path", result.artifacts.input_path.string()},
        {"preset_name", result.artifacts.config.preset_name},
        {"backend", result.backend_name},
        {"mask_rle_encoding", "row_major_start_length"},
        {"records", std::move(records)},
    };
    if (request.source_kind == PredictSourceKind::CompiledDataset) {
        output["compiled_path"] = request.compiled_path.string();
    } else {
        output["input_image_count"] = request.image_inputs.size();
    }
    if (!result.artifacts.weights_path.empty()) { output["weights_path"] = result.artifacts.weights_path.string(); }
    if (!result.artifacts.onnx_path.empty()) { output["onnx_path"] = result.artifacts.onnx_path.string(); }
    if (!result.artifacts.tensorrt_path.empty()) { output["tensorrt_path"] = result.artifacts.tensorrt_path.string(); }

    std::ofstream stream(request.output_path);
    if (!stream.is_open()) { throw std::runtime_error("failed to open RF-DETR prediction output: " + request.output_path.string()); }
    stream << output.dump(2) << '\n';
}

void print_prediction_summary(const PredictRequest&, const PredictionRunResult& result) {
    std::cout << result.backend_name << ": " << result.processed_images << " images, " << result.timing.img_per_s << " img/s\n";
}

}  // namespace mmltk::backend::models::rfdetr
