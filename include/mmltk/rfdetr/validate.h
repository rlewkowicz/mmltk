#pragma once

#include "mmltk/rfdetr/evaluation.h"
#include "mmltk/rfdetr/model_info.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mmltk::rfdetr {

enum class ValidationLogMode : std::uint8_t {
    Quiet,
    Interactive,
};

struct ValidationSharedConfig {
    std::string cpu_affinity;
    int device_id = 0;
    int workers = 0;
    bool recompile = false;
    bool profile = false;
    bool allow_fp16 = true;
    bool write_report_json = true;
};

template <typename PathType, typename ResolutionType, typename CountType>
struct ValidationDatasetConfig {
    PathType compiled_path;
    PathType source_dir;
    PathType onnx_path;
    PathType tensorrt_path;
    PathType save_engine_path;
    std::string eval_order = "onnx,tensorrt";
    ResolutionType resolution = static_cast<ResolutionType>(432);
    CountType limit_images = static_cast<CountType>(0);
    CountType alignment_images = static_cast<CountType>(16);
    CountType eval_max_dets = static_cast<CountType>(500);
    CountType batch_size = static_cast<CountType>(1);
    CountType prefetch_factor = static_cast<CountType>(2);
};

struct ValidationCompileConfig {
    int compile_workers = -1;
    int compile_cuda_mask_batch_size = 0;
    int compile_cuda_device_id = 0;
    ValidationLogMode log_mode = ValidationLogMode::Interactive;
};

using ValidationOptionsDatasetConfig = ValidationDatasetConfig<std::filesystem::path, std::uint32_t, std::size_t>;

struct ValidationOptions : ValidationSharedConfig, ValidationOptionsDatasetConfig, ValidationCompileConfig {
    std::filesystem::path report_json_path;
    std::string split;
};

struct PhaseTiming {
    double seconds = 0.0;
    double img_per_s = 0.0;
    size_t images = 0;
};

struct ValidationBackendResult {
    ModelInfo model_info;
    EvalSummary summary;
    PhaseTiming timing;
};

struct ValidationDeltaSummary {
    double bbox_ap = 0.0;
    double bbox_ap50 = 0.0;
    double mask_ap = 0.0;
    double mask_ap50 = 0.0;
};

struct ValidationRunResult {
    size_t images = 0;
    size_t categories = 0;
    std::vector<std::string> eval_order;
    std::unordered_map<std::string, ValidationBackendResult> backends;
    std::optional<AlignmentStats> alignment_probe;
    std::optional<ValidationDeltaSummary> delta_tensorrt_minus_onnx;
    std::optional<PhaseTiming> total_timing;
};

ValidationRunResult run_validation(const ValidationOptions& options);
void write_validation_report(const ValidationOptions& options, const ValidationRunResult& result);
void print_model_metadata(const ModelInfo& info, size_t images, size_t categories, ValidationLogMode log_mode);
void print_validation_run_summary(const ValidationOptions& options, const ValidationRunResult& result);

}  // namespace mmltk::rfdetr
