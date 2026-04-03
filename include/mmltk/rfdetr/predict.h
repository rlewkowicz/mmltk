#pragma once

#include "mmltk/rfdetr/artifacts.h"
#include "mmltk/rfdetr/evaluation.h"
#include "mmltk/rfdetr/validate.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mmltk::rfdetr {

enum class PredictSourceKind : std::uint8_t {
    CompiledDataset = 0,
    ImageFiles = 1,
};

struct PredictImageInput {
    std::filesystem::path image_path;
    std::string source_name;
    int64_t image_id = 0;
};

struct PredictOptions : ModelArtifactRequest {
    PredictSourceKind source_kind = PredictSourceKind::CompiledDataset;
    std::filesystem::path compiled_path;
    std::vector<PredictImageInput> image_inputs;
    std::filesystem::path output_path;
    std::string backend = "auto";
    size_t batch_size = 1;
    size_t max_dets_per_image = 500;
    int device_id = 0;
    int workers = 0;
    int lanes = 0;
    float threshold = 0.0f;
    std::string cpu_affinity;
    bool allow_fp16 = true;
    bool progress_bar = true;
    CompilationMode compilation_mode = CompilationMode::kSelective;
};

struct EvaluateOptions : ModelArtifactRequest {
    std::filesystem::path compiled_path;
    std::string backend = "auto";
    size_t batch_size = 1;
    size_t limit_images = 0;
    size_t eval_max_dets = 500;
    int device_id = 0;
    int workers = 0;
    int lanes = 0;
    std::string cpu_affinity;
    bool allow_fp16 = true;
    bool progress_bar = true;
    CompilationMode compilation_mode = CompilationMode::kSelective;
};

struct PredictionRecord {
    int64_t dataset_index = 0;
    int64_t image_id = 0;
    std::string source_name;
    std::vector<Prediction> detections;
};

struct PredictionRunResult {
    ResolvedModelArtifacts artifacts;
    std::string backend_name;
    std::vector<std::string> class_names;
    std::vector<PredictionRecord> records;
    PhaseTiming timing;
};

struct EvaluationRunResult {
    ResolvedModelArtifacts artifacts;
    std::string backend_name;
    size_t image_count = 0;
    size_t category_count = 0;
    ValidationBackendResult result;
};

PredictionRunResult run_prediction(const PredictOptions& options);
void write_prediction_json(const PredictOptions& options, const PredictionRunResult& result);
void print_prediction_summary(const PredictOptions& options, const PredictionRunResult& result);

EvaluationRunResult run_evaluation(const EvaluateOptions& options);
void print_evaluation_summary(const EvaluateOptions& options, const EvaluationRunResult& result);

} // namespace mmltk::rfdetr
